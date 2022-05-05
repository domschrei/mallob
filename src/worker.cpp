
#include <cmath>
#include <thread>
#include <unistd.h>
#include <iostream>
#include <fstream>
#include <initializer_list>
#include <limits>

#include "worker.hpp"

#include "balancing/event_driven_balancer.hpp"
#include "data/serializable.hpp"
#include "data/job_description.hpp"
#include "util/sys/process.hpp"
#include "util/sys/proc.hpp"
#include "util/sys/timer.hpp"
#include "util/sys/watchdog.hpp"
#include "util/logger.hpp"
#include "util/random.hpp"
#include "data/job_reader.hpp"
#include "util/sys/terminator.hpp"
#include "util/sys/thread_pool.hpp"

Worker::Worker(MPI_Comm comm, Parameters& params) :
    _comm(comm), _world_rank(MyMpi::rank(MPI_COMM_WORLD)), 
    _params(params), _job_db(_params, _comm, _sys_state), _sys_state(_comm, params.sysstatePeriod(), SysState<9>::ALLREDUCE), 
    _watchdog(/*enabled=*/_params.watchdog(), /*checkIntervMillis=*/100, Timer::elapsedSeconds())
{
    _watchdog.setWarningPeriod(50); // warn after 50ms without a reset
    _watchdog.setAbortPeriod(_params.watchdogAbortMillis()); // abort after X ms without a reset

    // Set callback which is called whenever a job's volume is updated
    _job_db.setBalancerVolumeUpdateCallback([&](int jobId, int volume, float eventLatency) {
        updateVolume(jobId, volume, _job_db.getGlobalBalancingEpoch(), eventLatency);
    });
    // Set callback for whenever a new balancing has been concluded
    _job_db.setBalancingDoneCallback([&]() {
        // apply any job requests which have arrived from a "future epoch"
        // which has now become the present (or a past) epoch
        for (auto& h : _job_db.getArrivedFutureRequests()) {
            LOG_ADD_SRC(V4_VVER, "From the future: tag=%i", h.source, h.tag);
            handleRequestNode(h, h.tag == MSG_REQUEST_NODE ? 
                JobDatabase::JobRequestMode::NORMAL : 
                JobDatabase::JobRequestMode::TARGETED_REJOIN);
        }
    });
}

void Worker::init() {
    
    // Initialize pseudo-random order of nodes
    if (_params.derandomize()) {
        createExpanderGraph();
    }

    auto& q = MyMpi::getMessageQueue();
    
    // Write tag of currently handled message into watchdog
    q.setCurrentTagPointers(_watchdog.activityRecvTag(), _watchdog.activitySendTag());

    q.registerSentCallback(MSG_SEND_JOB_DESCRIPTION, [&](int sendId) {
        auto it = _send_id_to_job_id.find(sendId);
        if (it != _send_id_to_job_id.end()) {
            int jobId = it->second;
            if (_job_db.has(jobId)) {
                _job_db.get(jobId).getJobTree().clearSendHandle(sendId);
            }
            _send_id_to_job_id.erase(sendId);
        }
    });

    // Begin listening to incoming messages
    q.registerCallback(MSG_ANSWER_ADOPTION_OFFER,
        [&](auto& h) {handleAnswerAdoptionOffer(h);});
    q.registerCallback(MSG_NOTIFY_JOB_ABORTING, 
        [&](auto& h) {handleNotifyJobAborting(h);});
    q.registerCallback(MSG_NOTIFY_JOB_TERMINATING, 
        [&](auto& h) {handleNotifyJobTerminating(h);});
    q.registerCallback(MSG_NOTIFY_RESULT_FOUND, 
        [&](auto& h) {handleNotifyResultFound(h);});
    q.registerCallback(MSG_INCREMENTAL_JOB_FINISHED,
        [&](auto& h) {handleIncrementalJobFinished(h);});
    q.registerCallback(MSG_INTERRUPT,
        [&](auto& h) {handleInterrupt(h);});
    q.registerCallback(MSG_NOTIFY_NODE_LEAVING_JOB, 
        [&](auto& h) {handleNotifyNodeLeavingJob(h);});
    q.registerCallback(MSG_NOTIFY_RESULT_OBSOLETE, 
        [&](auto& h) {handleNotifyResultObsolete(h);});
    q.registerCallback(MSG_NOTIFY_VOLUME_UPDATE, 
        [&](auto& h) {handleNotifyVolumeUpdate(h);});
    q.registerCallback(MSG_OFFER_ADOPTION, 
        [&](auto& h) {handleOfferAdoption(h);});
    q.registerCallback(MSG_QUERY_JOB_DESCRIPTION,
        [&](auto& h) {handleQueryJobDescription(h);});
    q.registerCallback(MSG_QUERY_JOB_RESULT, 
        [&](auto& h) {handleQueryJobResult(h);});
    q.registerCallback(MSG_QUERY_VOLUME, 
        [&](auto& h) {handleQueryVolume(h);});
    q.registerCallback(MSG_REJECT_ONESHOT, 
        [&](auto& h) {handleRejectOneshot(h);});
    q.registerCallback(MSG_REQUEST_NODE, 
        [&](auto& h) {handleRequestNode(h, JobDatabase::JobRequestMode::NORMAL);});
    q.registerCallback(MSG_REQUEST_NODE_ONESHOT, 
        [&](auto& h) {handleRequestNode(h, JobDatabase::JobRequestMode::TARGETED_REJOIN);});
    q.registerCallback(MSG_SEND_APPLICATION_MESSAGE, 
        [&](auto& h) {handleSendApplicationMessage(h);});
    q.registerCallback(MSG_JOB_TREE_REDUCTION, 
        [&](auto& h) {handleSendApplicationMessage(h);});
    q.registerCallback(MSG_JOB_TREE_BROADCAST, 
        [&](auto& h) {handleSendApplicationMessage(h);});
    q.registerCallback(MSG_SEND_JOB_DESCRIPTION, 
        [&](auto& h) {handleSendJobDescription(h);});
    q.registerCallback(MSG_NOTIFY_ASSIGNMENT_UPDATE, 
        [&](auto& h) {_coll_assign.handle(h);});
    q.registerCallback(MSG_SCHED_RELEASE_FROM_WAITING, 
        [&](auto& h) {handleSchedReleaseFromWaiting(h);});
    q.registerCallback(MSG_SCHED_NODE_FREED, 
        [&](auto& h) {handleSchedNodeFreed(h);});
    q.registerCallback(MSG_WARMUP, [&](auto& h) {
        LOG_ADD_SRC(V4_VVER, "Received warmup msg", h.source);
    });
    
    // Balancer message handling
    auto balanceCb = [&](MessageHandle& handle) {
        _job_db.handleBalancingMessage(handle);
    };
    q.registerCallback(MSG_COLLECTIVE_OPERATION, balanceCb);
    q.registerCallback(MSG_REDUCE_DATA, balanceCb);
    q.registerCallback(MSG_BROADCAST_DATA, balanceCb);
    
    // Local scheduler message handling
    auto localSchedulerCb = [&](MessageHandle& handle) {
        auto [jobId, index] = Serializable::get<IntPair>(handle.getRecvData());
        if (_job_db.hasScheduler(jobId, index)) 
            _job_db.getScheduler(jobId, index).handle(handle);
        else if (handle.tag == MSG_SCHED_INITIALIZE_CHILD_WITH_NODES) {
            // A scheduling package for an unknown job arrived:
            // it is important to return this package to the sender
            JobSchedulingUpdate update; update.deserialize(handle.getRecvData());
            update.destinationIndex = (update.destinationIndex-1) / 2;
            MyMpi::isend(handle.source, MSG_SCHED_RETURN_NODES, update);
        }
    };
    q.registerCallback(MSG_SCHED_INITIALIZE_CHILD_WITH_NODES, localSchedulerCb);
    q.registerCallback(MSG_SCHED_RETURN_NODES, localSchedulerCb);

    // Send warm-up messages with your pseudorandom bounce destinations
    if (_params.derandomize() && _params.warmup()) {
        IntVec payload({1, 2, 3, 4, 5, 6, 7, 8});
        for (auto rank : _hop_destinations) {
            MyMpi::isend(rank, MSG_WARMUP, payload);
            LOG_ADD_DEST(V4_VVER, "Sending warmup msg", rank);
            MyMpi::getMessageQueue().advance();
        }
    }
}

void Worker::createExpanderGraph() {

    // Pick fixed number k of bounce destinations
    int numBounceAlternatives = _params.numBounceAlternatives();
    int numWorkers = MyMpi::size(_comm);
    
    // Check validity of num bounce alternatives
    if (2*numBounceAlternatives > numWorkers) {
        numBounceAlternatives = std::max(1, numWorkers / 2);
        LOG(V1_WARN, "[WARN] Num bounce alternatives must be at most half the number of workers!\n");
        LOG(V1_WARN, "[WARN] Falling back to safe value r=%i.\n", numBounceAlternatives);
    }  

    // Create graph, get outgoing edges from this node
    if (_params.maxIdleDistance() > 0) {
        _hop_destinations = AdjustablePermutation::createUndirectedExpanderGraph(numWorkers, numBounceAlternatives, _world_rank);        
    } else {
        auto permutations = AdjustablePermutation::getPermutations(numWorkers, numBounceAlternatives);
        _hop_destinations = AdjustablePermutation::createExpanderGraph(permutations, _world_rank);
        if (_params.hopsUntilCollectiveAssignment() >= 0) {
            
            // Create collective assignment structure
            _coll_assign = CollectiveAssignment(
                _job_db, MyMpi::size(_comm), 
                AdjustablePermutation::getBestOutgoingEdgeForEachNode(permutations, _world_rank),
                // Callback for receiving a job request
                [&](const JobRequest& req, int rank) {
                    MessageHandle handle;
                    handle.tag = MSG_REQUEST_NODE;
                    handle.finished = true;
                    handle.receiveSelfMessage(req.serialize(), rank);
                    handleRequestNode(handle, JobDatabase::NORMAL);
                }
            );
            _job_db.setCollectiveAssignment(_coll_assign);
        }
    }

    // Output found bounce alternatives
    std::string info = "";
    for (size_t i = 0; i < _hop_destinations.size(); i++) {
        info += std::to_string(_hop_destinations[i]) + " ";
    }
    LOG(V3_VERB, "My bounce alternatives: %s\n", info.c_str());
    assert((int)_hop_destinations.size() == numBounceAlternatives);
}

void Worker::advance(float time) {

    // Timestamp provided?
    if (time < 0) time = Timer::elapsedSeconds();

    // Reset watchdog
    _watchdog.reset(time);
    
    // Check & print stats
    if (_periodic_stats_check.ready(time)) {
        _watchdog.setActivity(Watchdog::STATS);
        checkStats(time);
    }

    // Advance load balancing operations
    if (_periodic_balance_check.ready(time)) {
        _watchdog.setActivity(Watchdog::BALANCING);
        _job_db.advanceBalancing(time);

        // Advance collective assignment of nodes
        if (_params.hopsUntilCollectiveAssignment() >= 0) {
            _watchdog.setActivity(Watchdog::COLLECTIVE_ASSIGNMENT);
            _coll_assign.advance(_job_db.getGlobalBalancingEpoch());
        }
    }

    // Do diverse periodic maintenance tasks
    if (_periodic_maintenance.ready(time)) {
        
        // Forget jobs that are old or wasting memory
        _watchdog.setActivity(Watchdog::FORGET_OLD_JOBS);
        _job_db.forgetOldJobs();

        // Continue to bounce requests which were deferred earlier
        _watchdog.setActivity(Watchdog::THAW_JOB_REQUESTS);
        for (auto& [req, senderRank] : _job_db.getDeferredRequestsToForward(time)) {
            bounceJobRequest(req, senderRank);
        }
    }

    // Check jobs
    if (_periodic_job_check.ready(time)) {
        _watchdog.setActivity(Watchdog::CHECK_JOBS);
        checkJobs();
    }

    // Advance an all-reduction of the current system state
    if (_sys_state.aggregate(time)) {
        _watchdog.setActivity(Watchdog::SYSSTATE);
        publishAndResetSysState();
    }

    _watchdog.setActivity(Watchdog::IDLE_OR_HANDLING_MSG);
}

void Worker::checkStats(float time) {

    // For this process and subprocesses
    if (_node_stats_calculated.load(std::memory_order_acquire)) {
        
        // Update local sysstate, log update
        _sys_state.setLocal(SYSSTATE_GLOBALMEM, _node_memory_gbs);
        LOG(V4_VVER, "mem=%.2fGB mt_cpu=%.3f mt_sys=%.3f\n", _node_memory_gbs, _mainthread_cpu_share, _mainthread_sys_share);

        // Update host-internal communicator
        if (_host_comm) {
            _host_comm->setRamUsageThisWorkerGbs(_node_memory_gbs);
            _host_comm->setFreeAndTotalMachineMemoryKbs(_machine_free_kbs, _machine_total_kbs);
            if (_job_db.hasActiveJob()) {
                _host_comm->setActiveJobIndex(_job_db.getActive().getIndex());
            } else {
                _host_comm->unsetActiveJobIndex();
            }
        }

        // Recompute stats for next query time
        // (concurrently because computation of PSS is expensive)
        _node_stats_calculated.store(false, std::memory_order_relaxed);
        auto tid = Proc::getTid();
        ProcessWideThreadPool::get().addTask([&, tid]() {
            auto memoryKbs = Proc::getRecursiveProportionalSetSizeKbs(Proc::getPid());
            auto memoryGbs = memoryKbs / 1024.f / 1024.f;
            _node_memory_gbs = memoryGbs;
            Proc::getThreadCpuRatio(tid, _mainthread_cpu_share, _mainthread_sys_share);
            auto [freeKbs, totalKbs] = Proc::getMachineFreeAndTotalRamKbs();
            _machine_free_kbs = freeKbs;
            _machine_total_kbs = totalKbs;
            _node_stats_calculated.store(true, std::memory_order_release);
        });
    }

    // Print further stats?
    if (_periodic_big_stats_check.ready(time)) {

        // For the current job
        if (_job_db.hasActiveJob()) {
            Job& job = _job_db.getActive();
            job.appl_dumpStats();
            if (job.getJobTree().isRoot()) {
                std::string commStr = "";
                for (size_t i = 0; i < job.getJobComm().size(); i++) {
                    commStr += " " + std::to_string(job.getJobComm()[i]);
                }
                if (!commStr.empty()) LOG(V4_VVER, "%s job comm:%s\n", job.toStr(), commStr.c_str());
            }
        }
    }

    if (_host_comm && _host_comm->advanceAndCheckMemoryPanic(time)) {
        // Memory panic!
        // Aggressively remove inactive cached jobs
        _job_db.setMemoryPanic(true);
        _job_db.forgetOldJobs();
        _job_db.setMemoryPanic(false);
        // Trigger memory panic in the active job
        if (_job_db.hasActiveJob()) _job_db.getActive().appl_memoryPanic();
    }
}

void Worker::checkJobs() {

    // Load and try to adopt pending root reactivation request
    if (_job_db.hasPendingRootReactivationRequest()) {
        MessageHandle handle;
        handle.tag = MSG_REQUEST_NODE;
        handle.finished = true;
        handle.receiveSelfMessage(_job_db.loadPendingRootReactivationRequest().serialize(), _world_rank);
        handleRequestNode(handle, JobDatabase::NORMAL);
    }

    if (!_job_db.hasActiveJob()) {
        if (_job_db.isBusyOrCommitted()) {
            // PE is committed but not active
            _sys_state.setLocal(SYSSTATE_BUSYRATIO, 1.0f); // busy nodes
            _sys_state.setLocal(SYSSTATE_COMMITTEDRATIO, 1.0f); // committed nodes
        } else {
            // PE is completely idle
            _sys_state.setLocal(SYSSTATE_BUSYRATIO, 0.0f); // busy nodes
            _sys_state.setLocal(SYSSTATE_COMMITTEDRATIO, 0.0f); // committed nodes
        }
        _sys_state.setLocal(SYSSTATE_NUMJOBS, 0.0f); // active jobs
    } else {
        checkActiveJob();
    }
}

void Worker::checkActiveJob() {
    
    // PE runs an active job
    Job &job = _job_db.getActive();
    int id = job.getId();
    bool isRoot = job.getJobTree().isRoot();

    _sys_state.setLocal(SYSSTATE_BUSYRATIO, 1.0f); // busy nodes
    _sys_state.setLocal(SYSSTATE_COMMITTEDRATIO, 0.0f); // committed nodes
    _sys_state.setLocal(SYSSTATE_NUMJOBS, isRoot ? 1.0f : 0.0f); // active jobs

    bool abort = false;
    if (isRoot) abort = _job_db.checkComputationLimits(id);
    if (abort) {
        // Timeout (CPUh or wallclock time) hit
        timeoutJob(id);
    } else if (job.getState() == ACTIVE) {
        
        // Check if a result was found
        int result = job.appl_solved();
        if (result >= 0) {
            // Solver done!
            // Signal notification to root -- may be a self message
            int jobRootRank = job.getJobTree().getRootNodeRank();
            IntVec payload;
            {
                auto& resultStruct = job.getResult();
                assert(resultStruct.id == id);
                auto key = std::pair<int, int>(id, resultStruct.revision);
                payload = IntVec({id, resultStruct.revision, result});
                LOG_ADD_DEST(V4_VVER, "%s rev. %i: sending finished info", jobRootRank, job.toStr(), key.second);
                _pending_results[key] = std::move(resultStruct);
            }
            MyMpi::isend(jobRootRank, MSG_NOTIFY_RESULT_FOUND, payload);
        }

        // Update demand as necessary
        if (isRoot) {
            int demand = job.getDemand();
            if (demand != job.getLastDemand()) {
                // Demand updated
                _job_db.handleDemandUpdate(job, demand);
            }
        }

        // Handle child PEs waiting for the transfer of a revision of this job
        auto& waitingRankRevPairs = job.getWaitingRankRevisionPairs();
        auto it = waitingRankRevPairs.begin();
        while (it != waitingRankRevPairs.end()) {
            auto& [rank, rev] = *it;
            if (rev > job.getRevision()) {
                ++it;
                continue;
            }
            if (job.getJobTree().hasLeftChild() && job.getJobTree().getLeftChildNodeRank() == rank) {
                // Left child
                sendRevisionDescription(id, rev, rank);
            } else if (job.getJobTree().hasRightChild() && job.getJobTree().getRightChildNodeRank() == rank) {
                // Right child
                sendRevisionDescription(id, rev, rank);
            } // else: obsolete request
            // Remove processed request
            it = waitingRankRevPairs.erase(it);
        }
    }

    // Job communication (e.g. clause sharing)
    job.communicate();
}

void Worker::publishAndResetSysState() {

    if (_world_rank == 0) {
        const auto& result = _sys_state.getGlobal();
        int numDesires = result[SYSSTATE_NUMDESIRES];
        int numFulfilledDesires = result[SYSSTATE_NUMFULFILLEDDESIRES];
        float ratioFulfilled = numDesires <= 0 ? 0 : (float)numFulfilledDesires / numDesires;
        float latency = numFulfilledDesires <= 0 ? 0 : result[SYSSTATE_SUMDESIRELATENCIES] / numFulfilledDesires;

        LOG(V2_INFO, "sysstate busyratio=%.3f cmtdratio=%.3f jobs=%i globmem=%.2fGB newreqs=%i hops=%i\n", 
                    result[SYSSTATE_BUSYRATIO]/MyMpi::size(_comm), result[SYSSTATE_COMMITTEDRATIO]/MyMpi::size(_comm), 
                    (int)result[SYSSTATE_NUMJOBS], result[SYSSTATE_GLOBALMEM], (int)result[SYSSTATE_SPAWNEDREQUESTS], 
                    (int)result[SYSSTATE_NUMHOPS]);
    }
    
    if (!_job_db.isBusyOrCommitted()) {
        LOG(V4_VVER, "I am idle\n");
    }

    // Reset fields which are added to incrementally
    _sys_state.setLocal(SYSSTATE_NUMHOPS, 0);
    _sys_state.setLocal(SYSSTATE_SPAWNEDREQUESTS, 0);
    _sys_state.setLocal(SYSSTATE_NUMDESIRES, 0);
    _sys_state.setLocal(SYSSTATE_NUMFULFILLEDDESIRES, 0);
    _sys_state.setLocal(SYSSTATE_SUMDESIRELATENCIES, 0);
}

void Worker::handleNotifyJobAborting(MessageHandle& handle) {

    int jobId = Serializable::get<int>(handle.getRecvData());
    if (!_job_db.has(jobId)) return;

    LOG(V3_VERB, "Acknowledge #%i aborting\n", jobId);
    auto& job = _job_db.get(jobId);    
    if (job.getJobTree().isRoot()) {
        // Forward information on aborted job to client
        MyMpi::isend(job.getJobTree().getParentNodeRank(), 
            MSG_NOTIFY_CLIENT_JOB_ABORTING, handle.moveRecvData());
    }

    if (job.isIncremental()) {
        interruptJob(jobId, /*terminate=*/false, /*reckless=*/false);
    } else {
        interruptJob(jobId, /*terminate=*/true, /*reckless=*/true);
    }
}

void Worker::handleAnswerAdoptionOffer(MessageHandle& handle) {

    IntVec vec = Serializable::get<IntVec>(handle.getRecvData());
    int jobId = vec[0];
    int requestedNodeIndex = vec[1];
    bool accepted = vec[2] == 1;

    // Retrieve according job commitment
    if (!_job_db.hasCommitment(jobId)) {
        LOG(V4_VVER, "Job commitment for #%i not present despite adoption accept msg\n", jobId);
        return;
    }
    const JobRequest& req = _job_db.getCommitment(jobId);

    if (req.requestedNodeIndex != requestedNodeIndex || req.requestingNodeRank != handle.source) {
        // Adoption offer answer from invalid rank and/or index
        LOG_ADD_SRC(V4_VVER, "Ignore invalid adoption offer answer concerning #%i:%i\n", 
            handle.source, jobId, requestedNodeIndex);
        return;
    }

    // Retrieve job
    assert(_job_db.has(jobId));
    Job &job = _job_db.get(jobId);

    if (accepted) {
        // Adoption offer accepted
    
        // Check and apply (if possible) the job's current volume
        initiateVolumeUpdate(req.jobId);
        if (!job.hasCommitment()) {
            // Job shrunk: Commitment cancelled, abort job adoption
            return;
        }

        job.setDesiredRevision(req.revision);
        if (!job.hasDescription() || job.getRevision() < req.revision) {
            // Transfer of at least one revision is required
            int requestedRevision = job.hasDescription() ? job.getRevision()+1 : 0;
            MyMpi::isend(handle.source, MSG_QUERY_JOB_DESCRIPTION, IntPair(jobId, requestedRevision));
        }
        if (job.hasDescription()) {
            // At least the initial description is present: Begin to execute job
            _job_db.uncommit(req.jobId);
            if (job.getState() == SUSPENDED) {
                _job_db.reactivate(req, handle.source);
            } else {
                _job_db.execute(req.jobId, handle.source);
            }
        }
        
    } else {
        // Rejected
        LOG_ADD_SRC(V4_VVER, "Rejected to become %s : uncommitting", handle.source, job.toStr());
        _job_db.uncommit(req.jobId);
        _job_db.unregisterJobFromBalancer(req.jobId);
        _job_db.suspendScheduler(job);
    }
}

void Worker::handleQueryJobDescription(MessageHandle& handle) {
    IntPair pair = Serializable::get<IntPair>(handle.getRecvData());
    int jobId = pair.first;
    int revision = pair.second;

    if (!_job_db.has(jobId)) return;
    Job& job = _job_db.get(jobId);

    if (job.getRevision() >= revision) {
        sendRevisionDescription(jobId, revision, handle.source);
    } else {
        // This revision is not present yet: Defer this query
        // and send the job description upon receiving it
        job.addChildWaitingForRevision(handle.source, revision);
        return;
    }
}

void Worker::sendRevisionDescription(int jobId, int revision, int dest) {
    // Retrieve and send concerned job description
    auto& job = _job_db.get(jobId);
    const auto& descPtr = job.getSerializedDescription(revision);
    assert(descPtr->size() == job.getDescription().getTransferSize(revision) 
        || LOG_RETURN_FALSE("%i != %i\n", descPtr->size(), job.getDescription().getTransferSize(revision)));
    int sendId = MyMpi::isend(dest, MSG_SEND_JOB_DESCRIPTION, descPtr);
    LOG_ADD_DEST(V4_VVER, "Sent job desc. of %s rev. %i, size %lu, id=%i", dest, 
            job.toStr(), revision, descPtr->size(), sendId);
    job.getJobTree().addSendHandle(dest, sendId);
    _send_id_to_job_id[sendId] = jobId;
}

void Worker::handleRejectOneshot(MessageHandle& handle) {
    OneshotJobRequestRejection rej = Serializable::get<OneshotJobRequestRejection>(handle.getRecvData());
    JobRequest& req = rej.request;
    LOG_ADD_SRC(V5_DEBG, "%s rejected by dormant child", handle.source, 
            _job_db.toStr(req.jobId, req.requestedNodeIndex).c_str());

    if (!_job_db.has(req.jobId)) return;

    Job& job = _job_db.get(req.jobId);
    if (_params.reactivationScheduling() && _job_db.hasScheduler(req.jobId, job.getIndex())) {
        bool hasChild = req.requestedNodeIndex == job.getJobTree().getLeftChildIndex() ?
            job.getJobTree().hasLeftChild() : job.getJobTree().hasRightChild();
        _job_db.getScheduler(req.jobId, job.getIndex()).handleRejectReactivation(handle.source, req.balancingEpoch, 
            req.requestedNodeIndex, !rej.isChildStillDormant, hasChild);
        return;
    }

    if (_job_db.isAdoptionOfferObsolete(req)) return;

    if (!rej.isChildStillDormant) {
        job.getJobTree().removeDormantChild(handle.source);
    }

    bool doNormalHopping = false;
    if (req.numHops > std::max(_params.jobCacheSize(), 2)) {
        // Oneshot node finding exceeded
        doNormalHopping = true;
    } else {
        // Attempt another oneshot request
        // Get dormant children without the node that just declined
        int rank = job.getJobTree().getRankOfNextDormantChild();
        if (rank < 0 || rank == handle.source) {
            // No fitting dormant children left
            doNormalHopping = true;
        } else {
            // Pick a dormant child, forward request
            req.numHops++;
            _sys_state.addLocal(SYSSTATE_NUMHOPS, 1);
            MyMpi::isend(rank, MSG_REQUEST_NODE_ONESHOT, req);
            LOG_ADD_DEST(V4_VVER, "%s : query dormant child", rank, job.toStr());
            _sys_state.addLocal(SYSSTATE_SPAWNEDREQUESTS, 1);
        }
    }

    if (doNormalHopping) {
        LOG(V4_VVER, "%s : switch to normal hops\n", job.toStr());
        req.numHops = -1;
        bounceJobRequest(req, handle.source);
    }
}

void Worker::handleRequestNode(MessageHandle& handle, JobDatabase::JobRequestMode mode) {

    JobRequest req = Serializable::get<JobRequest>(handle.getRecvData());

    // Discard request if it has become obsolete
    if (_job_db.isRequestObsolete(req)) {
        LOG_ADD_SRC(V3_VERB, "DISCARD %s mode=%i", handle.source, 
                req.toStr().c_str(), mode);
        if (_params.hopsUntilCollectiveAssignment() >= 0) _coll_assign.setStatusDirty();
        return;
    }

    // Root request for the first revision of a new job?
    if (req.requestedNodeIndex == 0 && req.numHops == 0 && req.revision == 0) {
        // Probe balancer for a free spot.
        _job_db.addRootRequest(std::move(req));
        return;
    }

    if (req.balancingEpoch > _job_db.getGlobalBalancingEpoch()) {
        // Job request is "from the future": defer it until it is from the present
        LOG(V4_VVER, "Defer future req. %s\n", req.toStr().c_str());
        _job_db.addFutureRequestMessage(req.balancingEpoch, std::move(handle));
        return;
    }

    // Explicit rejoin request from reactivation-based scheduling?
    if (_params.reactivationScheduling() && mode == JobDatabase::TARGETED_REJOIN) {
        // Mark job as having been notified of the current scheduling and that it is not further needed.
        if (_job_db.has(req.jobId)) _job_db.get(req.jobId).getJobTree().stopWaitingForReactivation(req.balancingEpoch);
        if (_params.hopsUntilCollectiveAssignment() >= 0) _coll_assign.setStatusDirty();
    }

    tryAdoptRequest(req, handle.source, mode);
}

void Worker::tryAdoptRequest(JobRequest& req, int source, JobDatabase::JobRequestMode mode) {

    // Decide whether to adopt the job.
    JobDatabase::AdoptionResult adoptionResult = JobDatabase::ADOPT_FROM_IDLE;
    int removedJob;
    if (_params.reactivationScheduling() && mode != JobDatabase::TARGETED_REJOIN 
            && _job_db.hasInactiveJobsWaitingForReactivation() && req.requestedNodeIndex > 0) {
        // In reactivation-based scheduling, block incoming requests if you are still waiting
        // for a notification from some job of which you have an inactive job node.
        // Does not apply for targeted requests!
        adoptionResult = JobDatabase::REJECT;
    } else {
        adoptionResult = _job_db.tryAdopt(req, mode, source, removedJob);
    }

    // Do I adopt the job?
    if (adoptionResult == JobDatabase::ADOPT_FROM_IDLE || adoptionResult == JobDatabase::ADOPT_REPLACE_CURRENT) {

        // Replaced the current job
        if (adoptionResult == JobDatabase::ADOPT_REPLACE_CURRENT) {
            Job& job = _job_db.get(removedJob);
            MyMpi::isend(job.getJobTree().getParentNodeRank(), MSG_NOTIFY_NODE_LEAVING_JOB, 
                IntVec({job.getId(), job.getIndex(), job.getJobTree().getRootNodeRank()}));
        }

        // Adoption takes place
        std::string jobstr = _job_db.toStr(req.jobId, req.requestedNodeIndex);
        LOG_ADD_SRC(V3_VERB, "ADOPT %s mode=%i", source, req.toStr().c_str(), mode);
        assert(!_job_db.isBusyOrCommitted() || LOG_RETURN_FALSE("Adopting a job, but not idle!\n"));

        // Commit on the job, send a request to the parent
        if (!_job_db.has(req.jobId)) {
            // Job is not known yet: create instance
            Job& job = _job_db.createJob(MyMpi::size(_comm), _world_rank, req.jobId, req.application);
        }
        _job_db.commit(req);
        if (_params.reactivationScheduling()) {
            _job_db.initScheduler(req, [this](const JobRequest& req, int tag, bool left, int dest) {
                sendJobRequest(req, tag, left, dest);
            });
        }
        MyMpi::isend(req.requestingNodeRank, 
            req.requestedNodeIndex == 0 ? MSG_OFFER_ADOPTION_OF_ROOT : MSG_OFFER_ADOPTION,
            req);

    } else if (adoptionResult == JobDatabase::REJECT) {
        
        // Job request was rejected
        if (req.requestedNodeIndex == 0 && _job_db.has(req.jobId) && _job_db.get(req.jobId).getJobTree().isRoot()) {
            // I have the dormant root of this request, but cannot adopt right now:
            // defer until I can (e.g., until a made commitment can be broken)
            LOG(V4_VVER, "Defer pending root reactivation %s\n", req.toStr().c_str());
            _job_db.setPendingRootReactivationRequest(std::move(req));
        } else if (mode == JobDatabase::TARGETED_REJOIN) {
            // Send explicit rejection message
            OneshotJobRequestRejection rej(req, _job_db.hasDormantJob(req.jobId));
            LOG_ADD_DEST(V5_DEBG, "REJECT %s myepoch=%i", source, 
                        req.toStr().c_str(), _job_db.getGlobalBalancingEpoch());
            MyMpi::isend(source, MSG_REJECT_ONESHOT, rej);
        } else if (mode == JobDatabase::NORMAL) {
            // Continue job finding procedure somewhere else
            bounceJobRequest(req, source);
        }
    }
}

void Worker::handleIncrementalJobFinished(MessageHandle& handle) {
    int jobId = Serializable::get<int>(handle.getRecvData());
    if (_job_db.has(jobId)) {
        LOG(V3_VERB, "Incremental job %s done\n", _job_db.get(jobId).toStr());
        interruptJob(Serializable::get<int>(handle.getRecvData()), /*terminate=*/true, /*reckless=*/false);
    }
}

void Worker::handleInterrupt(MessageHandle& handle) {
    interruptJob(Serializable::get<int>(handle.getRecvData()), /*terminate=*/false, /*reckless=*/false);
}

void Worker::handleSendApplicationMessage(MessageHandle& handle) {

    // Deserialize job-specific message
    JobMessage msg = Serializable::get<JobMessage>(handle.getRecvData());

    int jobId = msg.jobId;
    if (!_job_db.has(jobId)) {
        LOG(V1_WARN, "[WARN] Job message from unknown job #%i\n", jobId);
        if (!msg.returnedToSender) {
            msg.returnedToSender = true;
            MyMpi::isend(handle.source, handle.tag, msg);
        }
        return;
    }

    // Give message to corresponding job
    Job& job = _job_db.get(jobId);
    job.communicate(handle.source, handle.tag, msg);
}

void Worker::handleOfferAdoption(MessageHandle& handle) {

    JobRequest req = Serializable::get<JobRequest>(handle.getRecvData());
    LOG_ADD_SRC(V4_VVER, "Adoption offer for %s", handle.source, 
                    _job_db.toStr(req.jobId, req.requestedNodeIndex).c_str());

    bool reject = false;
    if (!_job_db.has(req.jobId)) {
        // Job is not present, so I cannot become a parent!
        reject = true;

    } else {
        // Retrieve concerned job
        Job &job = _job_db.get(req.jobId);

        // Adoption offer is obsolete if it's internally obsolete or the job's scheduler declines it
        bool obsolete = _job_db.isAdoptionOfferObsolete(req);
        if (!obsolete) obsolete = _params.reactivationScheduling() 
            && _job_db.hasScheduler(job.getId(), job.getIndex()) 
            && !_job_db.getScheduler(job.getId(), job.getIndex()).acceptsChild(req.requestedNodeIndex);

        // Check if node should be adopted or rejected
        if (obsolete) {
            // Obsolete request
            LOG_ADD_SRC(V3_VERB, "REJECT %s", handle.source, req.toStr().c_str());
            reject = true;

        } else {
            // Adopt the job.
            // Child will start / resume its job solvers.
            // Mark new node as one of the node's children
            auto relative = job.getJobTree().setChild(handle.source, req.requestedNodeIndex);
            if (relative == JobTree::TreeRelative::NONE) assert(req.requestedNodeIndex == 0);
        }
    }

    // Answer the adoption offer
    MyMpi::isend(handle.source, MSG_ANSWER_ADOPTION_OFFER, 
        IntVec({req.jobId, req.requestedNodeIndex, reject ? 0 : 1}));

    // Triggers for reactivation-based scheduling
    if (_params.reactivationScheduling()) {
        
        // Retrieve local job scheduler if present
        if (!_job_db.has(req.jobId)) return;
        Job& job = _job_db.get(req.jobId);
        if (!_job_db.hasScheduler(req.jobId, job.getIndex())) return;
        auto& scheduler = _job_db.getScheduler(req.jobId, job.getIndex());

        if (!reject) {
            // Adoption accepted
            scheduler.handleChildJoining(handle.source, req.balancingEpoch, req.requestedNodeIndex);
        } else {
            // Adoption declined
            bool hasChild = req.requestedNodeIndex == job.getJobTree().getLeftChildIndex() ?
                job.getJobTree().hasLeftChild() : job.getJobTree().hasRightChild();
            scheduler.handleRejectReactivation(handle.source, req.balancingEpoch, 
                req.requestedNodeIndex, /*lost=*/false, hasChild);
        }
    }
}

void Worker::handleQueryJobResult(MessageHandle& handle) {

    // Receive acknowledgement that the client received the advertised result size
    // and wishes to receive the full job result
    JobStatistics stats; stats.deserialize(handle.getRecvData());
    int jobId = stats.jobId;
    auto key = std::pair<int, int>(jobId, stats.revision);
    LOG_ADD_DEST(V3_VERB, "Send result of #%i rev. %i to client", handle.source, jobId, stats.revision);
    assert(_pending_results.count(key));
    JobResult& result = _pending_results.at(key);
    assert(result.id == jobId);
    result.updateSerialization();
    MyMpi::isend(handle.source, MSG_SEND_JOB_RESULT, result.moveSerialization());
    _pending_results.erase(key);
}

void Worker::handleQueryVolume(MessageHandle& handle) {

    IntVec payload = Serializable::get<IntVec>(handle.getRecvData());
    int jobId = payload[0];

    // Unknown job? -- ignore.
    if (!_job_db.has(jobId)) return;

    Job& job = _job_db.get(jobId);
    int volume = job.getVolume();
    
    // Volume is unknown right now? Query parent recursively. 
    // (Answer will flood back to the entire subtree)
    if (job.getState() == ACTIVE && volume == 0) {
        assert(!job.getJobTree().isRoot());
        MyMpi::isendCopy(job.getJobTree().getParentNodeRank(), MSG_QUERY_VOLUME, handle.getRecvData());
        return;
    }

    // Send response
    IntVec response({jobId, volume, _job_db.getGlobalBalancingEpoch()});
    LOG_ADD_DEST(V4_VVER, "Answer #%i volume query with v=%i", handle.source, jobId, volume);
    MyMpi::isend(handle.source, MSG_NOTIFY_VOLUME_UPDATE, response);
}

void Worker::handleNotifyResultObsolete(MessageHandle& handle) {
    IntVec res = Serializable::get<IntVec>(handle.getRecvData());
    int jobId = res[0];
    int revision = res[1];
    LOG_ADD_SRC(V4_VVER, "job result for #%i rev. %i unwanted", handle.source, jobId, revision);
    _pending_results.erase(std::pair<int, int>(jobId, revision));
}

void Worker::handleSendJobDescription(MessageHandle& handle) {
    const auto& data = handle.getRecvData();
    int jobId = data.size() >= sizeof(int) ? Serializable::get<int>(data) : -1;
    LOG_ADD_SRC(V4_VVER, "Got desc. of size %i for job #%i", handle.source, data.size(), jobId);
    if (jobId == -1 || !_job_db.has(jobId)) {
        if (_job_db.hasCommitment(jobId)) {
            _job_db.uncommit(jobId);
            _job_db.unregisterJobFromBalancer(jobId);
            if (_job_db.has(jobId)) _job_db.suspendScheduler(_job_db.get(jobId));
        }
        return;
    }

    // Append revision description to job
    auto& job = _job_db.get(jobId);
    auto dataPtr = std::shared_ptr<std::vector<uint8_t>>(
        new std::vector<uint8_t>(handle.moveRecvData())
    );
    bool valid = _job_db.appendRevision(jobId, dataPtr, handle.source);
    if (!valid) {
        // Need to clean up shared pointer concurrently 
        // because it might take too much time in the main thread
        ProcessWideThreadPool::get().addTask([sharedPtr = std::move(dataPtr)]() mutable {
            sharedPtr.reset();
        });
        return;
    }

    // If job has not started yet, execute it now
    if (_job_db.hasCommitment(jobId)) {
        {
            const auto& req = _job_db.getCommitment(jobId);
            job.setDesiredRevision(req.revision);
            _job_db.uncommit(jobId);
        }
        _job_db.execute(jobId, handle.source);
        initiateVolumeUpdate(jobId);
    }
    
    // Job inactive?
    if (job.getState() != ACTIVE) return;

    // Arrived at final revision?
    if (_job_db.get(jobId).getRevision() < _job_db.get(jobId).getDesiredRevision()) {
        // No: Query next revision
        MyMpi::isend(handle.source, MSG_QUERY_JOB_DESCRIPTION, IntPair(jobId, _job_db.get(jobId).getRevision()+1));
    }
}

void Worker::handleNotifyJobTerminating(MessageHandle& handle) {
    interruptJob(Serializable::get<int>(handle.getRecvData()), /*terminate=*/true, /*reckless=*/false);
}

void Worker::handleNotifyVolumeUpdate(MessageHandle& handle) {
    IntVec recv = Serializable::get<IntVec>(handle.getRecvData());
    int jobId = recv[0];
    int volume = recv[1];
    int balancingEpoch = recv[2];
    if (!_job_db.has(jobId)) {
        LOG(V1_WARN, "[WARN] Volume update for unknown #%i\n", jobId);
        return;
    }

    // Update volume assignment in job instance (and its children)
    updateVolume(jobId, volume, balancingEpoch, 0);
}

void Worker::handleNotifyNodeLeavingJob(MessageHandle& handle) {

    // Retrieve job
    IntVec recv = Serializable::get<IntVec>(handle.getRecvData());
    int jobId = recv.data[0];
    int index = recv.data[1];
    int rootRank = recv.data[2];

    if (!_job_db.has(jobId)) {
        MyMpi::isend(rootRank, MSG_NOTIFY_NODE_LEAVING_JOB, handle.moveRecvData());
        return;
    }
    Job& job = _job_db.get(jobId);

    // Prune away the respective child if necessary
    auto pruned = job.getJobTree().prune(handle.source, index);

    // If necessary, find replacement
    if (pruned != JobTree::TreeRelative::NONE && index < job.getVolume()) {
        LOG(V4_VVER, "%s : look for replacement for %s\n", job.toStr(), _job_db.toStr(jobId, index).c_str());
        spawnJobRequest(jobId, pruned==JobTree::LEFT_CHILD, _job_db.getGlobalBalancingEpoch());
    }

    // Initiate communication if the job now became willing to communicate
    job.communicate();
}

void Worker::handleNotifyResultFound(MessageHandle& handle) {

    // Retrieve job
    IntVec res = Serializable::get<IntVec>(handle.getRecvData());
    int jobId = res[0];
    int revision = res[1];

    // Is the job result invalid or obsolete?
    bool obsolete = false;
    if (!_job_db.has(jobId) || !_job_db.get(jobId).getJobTree().isRoot()) {
        obsolete = true;
        LOG(V1_WARN, "[WARN] Invalid adressee for job result of #%i\n", jobId);
    } else if (_job_db.get(jobId).getRevision() > revision || _job_db.get(jobId).isRevisionSolved(revision)) {
        obsolete = true;
        LOG_ADD_SRC(V4_VVER, "Discard obsolete result for job #%i rev. %i", handle.source, jobId, revision);
    }
    if (obsolete) {
        MyMpi::isendCopy(handle.source, MSG_NOTIFY_RESULT_OBSOLETE, handle.getRecvData());
        return;
    }
    
    LOG_ADD_SRC(V3_VERB, "#%i rev. %i solved", handle.source, jobId, revision);
    _job_db.get(jobId).setRevisionSolved(revision);

    // Notify client
    sendJobDoneWithStatsToClient(jobId, revision, handle.source);

    // Terminate job and propagate termination message
    if (_job_db.get(jobId).getDescription().isIncremental()) {
        handleInterrupt(handle);
    } else {
        handleNotifyJobTerminating(handle);
    }
}

void Worker::handleSchedReleaseFromWaiting(MessageHandle& handle) {

    IntVec vec = Serializable::get<IntVec>(handle.getRecvData());
    int jobId = vec[0];
    int index = vec[1];
    int epoch = vec[2];
    if (_job_db.has(jobId) && 
            (_job_db.get(jobId).getState() != INACTIVE || _job_db.get(jobId).hasCommitment())) {
        // Job present: release this worker from waiting for that job
        _job_db.get(jobId).getJobTree().stopWaitingForReactivation(epoch);
        if (_params.hopsUntilCollectiveAssignment() >= 0) _coll_assign.setStatusDirty();
    } else {
        // Job not present any more: Let sender know
        MyMpi::isend(handle.source, MSG_SCHED_NODE_FREED, IntVec({jobId, _world_rank, index, epoch}));
    }
}

void Worker::handleSchedNodeFreed(MessageHandle& handle) {

    IntVec vec = Serializable::get<IntVec>(handle.getRecvData());
    int jobId = vec[0];
    int rank = vec[1];
    int index = vec[2];
    int epoch = vec[3];
    // If an active job scheduler is present, erase the freed node from local inactive nodes
    // (may not always succeed if the job has shrunk in the meantime, but is not essential for correctness)
    if (_job_db.hasScheduler(jobId, index)) {
        _job_db.getScheduler(jobId, index).handleNodeFreed(rank, epoch, index);
    }
}

void Worker::bounceJobRequest(JobRequest& request, int senderRank) {

    // Increment #hops
    request.numHops++;
    int num = request.numHops;
    _sys_state.addLocal(SYSSTATE_NUMHOPS, 1);

    // Show warning if #hops is a large power of two
    if ((num >= 512) && ((num & (num - 1)) == 0)) {
        LOG(V1_WARN, "[WARN] %s\n", request.toStr().c_str());
    }

    // If hopped enough for collective assignment to be enabled
    // and if either reactivation scheduling is employed or the requested node is non-root
    if (_params.hopsUntilCollectiveAssignment() >= 0 && num >= _params.hopsUntilCollectiveAssignment()
        && (_params.reactivationScheduling() || request.requestedNodeIndex > 0)) {
        _coll_assign.addJobRequest(request);
        return;
    }

    int nextRank;
    if (_params.derandomize()) {
        // Get random choice from bounce alternatives
        nextRank = getWeightedRandomNeighbor();
        if (_hop_destinations.size() > 2) {
            // ... if possible while skipping the requesting node and the sender
            while (nextRank == request.requestingNodeRank || nextRank == senderRank) {
                nextRank = getWeightedRandomNeighbor();
            }
        }
    } else {
        // Generate pseudorandom permutation of this request
        int n = MyMpi::size(_comm);
        AdjustablePermutation perm(n, 3 * request.jobId + 7 * request.requestedNodeIndex + 11 * request.requestingNodeRank);
        // Fetch next index of permutation based on number of hops
        int permIdx = request.numHops % n;
        nextRank = perm.get(permIdx);
        if (n > 3) {
            // ... if possible while skipping yourself, the requesting node, and the sender
            while (nextRank == _world_rank || nextRank == request.requestingNodeRank || nextRank == senderRank) {
                permIdx = (permIdx+1) % n;
                nextRank = perm.get(permIdx);
            }
        }
    }

    // Send request to "next" worker node
    LOG_ADD_DEST(V5_DEBG, "Hop %s", nextRank, _job_db.toStr(request.jobId, request.requestedNodeIndex).c_str());
    MyMpi::isend(nextRank, MSG_REQUEST_NODE, request);
}

void Worker::initiateVolumeUpdate(int jobId) {

    auto& job = _job_db.get(jobId);
    
    if (_params.explicitVolumeUpdates()) {
        // Volume updates are propagated explicitly
        if (job.getJobTree().isRoot()) {
            // Root worker: update volume (to trigger growth if desired)
            if (job.getVolume() > 1) updateVolume(jobId, job.getVolume(), _job_db.getGlobalBalancingEpoch(), 0);
        } else {
            // Non-root worker: query parent for the volume of this job
            IntVec payload({jobId});
            MyMpi::isend(job.getJobTree().getParentNodeRank(), MSG_QUERY_VOLUME, payload);
        }
    } else {
        // Volume updates are recognized by each job node independently
        if (_job_db.getGlobalBalancingEpoch() < job.getBalancingEpochOfLastCommitment()) {
            // Balancing epoch which caused this job node is not present yet
            return;
        }
        // Apply current volume
        if (_job_db.hasVolume(jobId)) {
            updateVolume(jobId, _job_db.getVolume(jobId), _job_db.getGlobalBalancingEpoch(), 0);
        }
    }
}

void Worker::updateVolume(int jobId, int volume, int balancingEpoch, float eventLatency) {

    // If the job is not in the database, there might be a root request to activate 
    if (!_job_db.has(jobId)) {
        activateRootRequest(jobId);
        return;
    }

    Job &job = _job_db.get(jobId);
    int thisIndex = job.getIndex();
    int prevVolume = job.getVolume();
    auto& tree = job.getJobTree();

    // Print out volume update with a certain verbosity
#define LOG_VOL_UPDATE(verb) LOG(verb, "%s : update v=%i epoch=%i lastreqsepoch=%i evlat=%.5f\n", \
job.toStr(), volume, balancingEpoch, tree.getBalancingEpochOfLastRequests(), eventLatency);
    if (job.getState() == ACTIVE && prevVolume != volume && thisIndex == 0) {
        LOG_VOL_UPDATE(V3_VERB)
    } else if (job.getState() == ACTIVE) {
        LOG_VOL_UPDATE(V4_VVER)
    } else {
        LOG_VOL_UPDATE(V5_DEBG)
    }
#undef LOG_VOL_UPDATE

    // Apply volume update to local job structure
    job.updateVolumeAndUsedCpu(volume);
    tree.stopWaitingForReactivation(balancingEpoch-1);
    
    if (job.getState() == ACTIVE || job.hasCommitment()) {

        if (_params.hopsUntilCollectiveAssignment() >= 0) _coll_assign.setStatusDirty();

        // Root of a job updated for the 1st time
        if (tree.isRoot()) {
            if (tree.getBalancingEpochOfLastRequests() == -1) {
                // Job's volume is updated for the first time
                job.setTimeOfFirstVolumeUpdate(Timer::elapsedSeconds());
            }
        }
        
        // Apply volume update to the job's local scheduler
        if (_params.reactivationScheduling() && _job_db.hasScheduler(jobId, job.getIndex()))
            _job_db.getScheduler(jobId, job.getIndex()).updateBalancing(balancingEpoch, volume, 
                tree.hasLeftChild(), tree.hasRightChild());

        // Handle child relationships with respect to the new volume
        propagateVolumeUpdate(job, volume, balancingEpoch);

        // Update balancing epoch
        tree.setBalancingEpochOfLastRequests(balancingEpoch);
        
        // Shrink (and pause solving) yourself, if necessary
        if (thisIndex > 0 && thisIndex >= volume) {
            LOG(V3_VERB, "%s shrinking\n", job.toStr());
            if (job.getState() == ACTIVE) {
                _job_db.suspend(jobId);
            } else {
                _job_db.uncommit(jobId);
                _job_db.unregisterJobFromBalancer(jobId);
                _job_db.suspendScheduler(job);
            }
            if (!_params.reactivationScheduling()) {
                // Send explicit leaving message if not doing reactivation-based scheduling
                MyMpi::isend(tree.getParentNodeRank(), MSG_NOTIFY_NODE_LEAVING_JOB, 
                    IntVec({jobId, thisIndex, tree.getRootNodeRank()}));
            }
        }

    } else if (job.getState() == SUSPENDED) {
        
        bool wasWaiting = tree.isWaitingForReactivation();
        // If the volume WAS and IS larger than my index and I WAS waiting,
        // then I will KEEP waiting.
        if (job.getIndex() < prevVolume && job.getIndex() < volume && wasWaiting) {
            tree.setWaitingForReactivation(balancingEpoch);
        }
        // If the volume WASN'T but now IS larger than my index,
        // then I will START waiting
        if (job.getIndex() >= prevVolume && job.getIndex() < volume) {
            tree.setWaitingForReactivation(balancingEpoch);
        }
    }
}

void Worker::activateRootRequest(int jobId) {
    auto optReq = _job_db.getRootRequest(jobId);
    if (!optReq.has_value()) return;
    LOG(V3_VERB, "Activate %s\n", optReq.value().toStr().c_str());
    bounceJobRequest(optReq.value(), optReq.value().requestingNodeRank);
}

void Worker::propagateVolumeUpdate(Job& job, int volume, int balancingEpoch) {

    // Prepare volume update to propagate down the job tree
    int jobId = job.getId();
    int thisIndex = job.getIndex();
    auto& tree = job.getJobTree();
    IntVec payload{jobId, volume, balancingEpoch};

    // For each potential child (left, right):
    bool has[2] = {tree.hasLeftChild(), tree.hasRightChild()};
    int indices[2] = {tree.getLeftChildIndex(), tree.getRightChildIndex()};
    int ranks[2] = {-1, -1};
    for (int i = 0; i < 2; i++) {
        int nextIndex = indices[i];
        if (has[i]) {
            ranks[i] = i == 0 ? tree.getLeftChildNodeRank() : tree.getRightChildNodeRank();
            if (_params.explicitVolumeUpdates()) {
                // Propagate volume update
                MyMpi::isend(ranks[i], MSG_NOTIFY_VOLUME_UPDATE, payload);
            }
            if (_params.reactivationScheduling() && nextIndex >= volume) {
                // Child leaves
                tree.prune(ranks[i], nextIndex);
            }
        } else if (nextIndex < volume 
                && tree.getBalancingEpochOfLastRequests() < balancingEpoch) {
            if (_job_db.hasDormantRoot()) {
                // Becoming an inner node is not acceptable
                // because then the dormant root cannot be restarted seamlessly
                LOG(V4_VVER, "%s cannot grow due to dormant root\n", job.toStr());
                if (job.getState() == ACTIVE) {
                    _job_db.suspend(jobId);
                } else {
                    _job_db.uncommit(jobId);
                    _job_db.unregisterJobFromBalancer(jobId);
                    _job_db.suspendScheduler(job);
                }
                MyMpi::isend(tree.getParentNodeRank(), 
                    MSG_NOTIFY_NODE_LEAVING_JOB, IntVec({jobId, thisIndex, tree.getRootNodeRank()}));
                break;
            }
            if (!_params.reactivationScheduling()) {
                // Try to grow immediately
                spawnJobRequest(jobId, i==0, balancingEpoch);
            }
        } else {
            // Job does not want to grow - any more (?) - so unset any previous desire
            if (i == 0) tree.unsetDesireLeft();
            else tree.unsetDesireRight();
        }
    }
}

void Worker::spawnJobRequest(int jobId, bool left, int balancingEpoch) {

    Job& job = _job_db.get(jobId);
    
    int index = left ? job.getJobTree().getLeftChildIndex() : job.getJobTree().getRightChildIndex();
    if (_params.monoFilename.isSet()) job.getJobTree().updateJobNode(index, index);

    JobRequest req(jobId, job.getApplication(), job.getJobTree().getRootNodeRank(), 
            _world_rank, index, Timer::elapsedSeconds(), balancingEpoch, 0);
    req.revision = job.getDesiredRevision();
    int tag = MSG_REQUEST_NODE;    

    sendJobRequest(req, tag, left, -1);
}

void Worker::sendJobRequest(const JobRequest& req, int tag, bool left, int dest) {

    auto& job = _job_db.get(req.jobId);

    if (dest == -1) {
        int nextNodeRank = job.getJobTree().getRankOfNextDormantChild(); 
        if (nextNodeRank < 0) {
            tag = MSG_REQUEST_NODE;
            nextNodeRank = left ? job.getJobTree().getLeftChildNodeRank() : job.getJobTree().getRightChildNodeRank();
        }
        dest = nextNodeRank;
    }

    LOG_ADD_DEST(V3_VERB, "%s growing: %s", dest, job.toStr(), req.toStr().c_str());
    
    MyMpi::isend(dest, tag, req);
    
    _sys_state.addLocal(SYSSTATE_SPAWNEDREQUESTS, 1);
    if (left) job.getJobTree().setDesireLeft(Timer::elapsedSeconds());
    else job.getJobTree().setDesireRight(Timer::elapsedSeconds());
}

void Worker::interruptJob(int jobId, bool terminate, bool reckless) {

    if (!_job_db.has(jobId)) return;
    Job& job = _job_db.get(jobId);

    // Ignore if this job node is already in the goal state
    // (also implying that it already forwarded such a request downwards if necessary)
    if (!terminate && !job.hasCommitment() && (job.getState() == SUSPENDED || job.getState() == INACTIVE)) 
        return;

    // Propagate message down the job tree
    int msgTag;
    if (terminate && reckless) msgTag = MSG_NOTIFY_JOB_ABORTING;
    else if (terminate) msgTag = MSG_NOTIFY_JOB_TERMINATING;
    else msgTag = MSG_INTERRUPT;
    if (job.getJobTree().hasLeftChild()) {
        MyMpi::isend(job.getJobTree().getLeftChildNodeRank(), msgTag, IntVec({jobId}));
        LOG_ADD_DEST(V4_VVER, "Propagate interruption of %s ...", job.getJobTree().getLeftChildNodeRank(), job.toStr());
    }
    if (job.getJobTree().hasRightChild()) {
        MyMpi::isend(job.getJobTree().getRightChildNodeRank(), msgTag, IntVec({jobId}));
        LOG_ADD_DEST(V4_VVER, "Propagate interruption of %s ...", job.getJobTree().getRightChildNodeRank(), job.toStr());
    }
    for (auto childRank : job.getJobTree().getPastChildren()) {
        MyMpi::isend(childRank, msgTag, IntVec({jobId}));
        LOG_ADD_DEST(V4_VVER, "Propagate interruption of %s (past child) ...", childRank, job.toStr());
    }
    if (terminate) job.getJobTree().getPastChildren().clear();

    // Uncommit job if committed
    if (job.hasCommitment()) {
        _job_db.uncommit(jobId);
        _job_db.unregisterJobFromBalancer(jobId);
        _job_db.suspendScheduler(job);
    }

    // Suspend or terminate the job
    if (terminate) _job_db.terminate(jobId);
    else if (job.getState() == ACTIVE) _job_db.suspend(jobId);
}

void Worker::sendJobDoneWithStatsToClient(int jobId, int revision, int successfulRank) {
    auto& job = _job_db.get(jobId);

    int clientRank = job.getDescription().getClientRank();
    LOG_ADD_DEST(V4_VVER, "%s : inform client job is done", clientRank, job.toStr());
    job.updateVolumeAndUsedCpu(job.getVolume());
    JobStatistics stats;
    stats.jobId = jobId;
    stats.revision = revision;
    stats.successfulRank = successfulRank;
    stats.usedWallclockSeconds = job.getAgeSinceActivation();
    stats.usedCpuSeconds = job.getUsedCpuSeconds();
    stats.latencyOf1stVolumeUpdate = job.getLatencyOfFirstVolumeUpdate();

    // Send "Job done!" with statistics to client
    MyMpi::isend(clientRank, MSG_NOTIFY_JOB_DONE, stats);
}

void Worker::timeoutJob(int jobId) {
    // "Virtual self message" aborting the job
    IntVec payload({jobId});
    MessageHandle handle;
    handle.tag = MSG_NOTIFY_JOB_ABORTING;
    handle.finished = true;
    handle.receiveSelfMessage(payload.serialize(), _world_rank);
    handleNotifyJobAborting(handle);
    if (_params.monoFilename.isSet()) {
        // Single job hit a limit, so there is no solution to be reported:
        // begin to propagate exit signal
        MyMpi::isend(0, MSG_DO_EXIT, IntVec({0}));
    }
}

int Worker::getWeightedRandomNeighbor() {
    int rand = (int) (_hop_destinations.size()*Random::rand());
    return _hop_destinations[rand];
}

int Worker::getRandomNonSelfWorkerNode() {
    int size = MyMpi::size(_comm);
    
    float r = Random::rand();
    int node = (int) (r * size);
    while (node == _world_rank) {
        r = Random::rand();
        node = (int) (r * size);
    }

    return node;
}

Worker::~Worker() {

    _watchdog.stop();
    Terminator::setTerminating();

    LOG(V4_VVER, "Destruct worker\n");

    if (_params.monoFilename.isSet() && _params.applicationSpawnMode() != "fork") {
        // Terminate directly without destructing resident job
        MPI_Finalize();
        Process::doExit(0);
    }
}