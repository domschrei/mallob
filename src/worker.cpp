
#include <cmath>
#include <thread>
#include <unistd.h>
#include <iostream>
#include <fstream>
#include <initializer_list>
#include <limits>

#include "worker.hpp"

#include "app/sat/threaded_sat_job.hpp"
#include "app/sat/forked_sat_job.hpp"
#include "app/sat/sat_constants.h"

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
    _params(params), _job_db(_params, _comm, _sys_state), _sys_state(_comm), 
    _bfs(_hop_destinations, [this](const JobRequest& req) {
        if (!_job_db.isBusyOrCommitted()) {
            // Try to adopt this job request
            log(V4_VVER, "BFS Try adopting %s\n", req.toStr().c_str());
            MessageHandle handle;
            handle.receiveSelfMessage(req.serialize(), _world_rank);
            handleRequestNode(handle, JobDatabase::IGNORE_FAIL);
            return _job_db.hasCommitment(req.jobId) ? _world_rank : -1;
        }
        return -1;
    }, [this](const JobRequest& request, int foundRank) {
        if (foundRank == -1) {
            log(V4_VVER, "%s : BFS unsuccessful - continue bouncing\n", request.toStr().c_str());
            JobRequest req = request;
            bounceJobRequest(req, _world_rank);
        } else {
            log(V4_VVER, "%s : BFS successful ([%i] adopting)\n", request.toStr().c_str(), foundRank);
        }
    }), _watchdog(/*checkIntervMillis=*/200, Timer::elapsedSeconds())
{
    _global_timeout = _params.timeLimit();
    _watchdog.setWarningPeriod(100); // warn after 0.1s without a reset
    _watchdog.setAbortPeriod(_params.watchdogAbortMillis()); // abort after X ms without a reset

    // Set callback which is called whenever a job's volume is updated
    _job_db.setBalancerVolumeUpdateCallback([&](int jobId, int volume, float eventLatency) {
        updateVolume(jobId, volume, _job_db.getGlobalBalancingEpoch(), eventLatency);
    });
}

void Worker::init() {
    
    // Initialize pseudo-random order of nodes
    if (_params.derandomize()) {
        createExpanderGraph();
    }

    // Begin listening to an incoming message
    auto& q = MyMpi::getMessageQueue();
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
    q.registerCallback(MSG_SEND_JOB_DESCRIPTION, 
        [&](auto& h) {handleSendJobDescription(h);});
    q.registerCallback(MSG_NOTIFY_NEIGHBOR_STATUS,
        [&](auto& h) {handleNotifyNeighborStatus(h);});
    q.registerCallback(MSG_NOTIFY_NEIGHBOR_IDLE_DISTANCE,
        [&](auto& h) {handleNotifyNeighborIdleDistance(h);});
    q.registerCallback(MSG_REQUEST_WORK,
        [&](auto& h) {handleRequestWork(h);});
    q.registerCallback(MSG_REQUEST_IDLE_NODE_BFS,
        [&](auto& h) {_bfs.handle(h);});
    q.registerCallback(MSG_ANSWER_IDLE_NODE_BFS,
        [&](auto& h) {_bfs.handle(h);});
    q.registerCallback(MSG_NOTIFY_ASSIGNMENT_UPDATE, 
        [&](auto& h) {_coll_assign.handle(h);});
    auto balanceCb = [&](MessageHandle& handle) {
        _job_db.handleBalancingMessage(handle);
    };
    q.registerCallback(MSG_COLLECTIVE_OPERATION, balanceCb);
    q.registerCallback(MSG_REDUCE_DATA, balanceCb);
    q.registerCallback(MSG_BROADCAST_DATA, balanceCb);
    q.registerCallback(MSG_WARMUP, [&](auto& h) {
        log(LOG_ADD_SRCRANK | V5_DEBG, "Warmup msg", h.source);
    });

    // Send warm-up messages with your pseudorandom bounce destinations
    if (_params.derandomize() && _params.warmup()) {
        IntVec payload({1, 2, 3, 4, 5, 6, 7, 8});
        int numRuns = 5;
        for (int run = 0; run < numRuns; run++) {
            for (auto rank : _hop_destinations) {
                MyMpi::isend(rank, MSG_WARMUP, payload);
                log(LOG_ADD_DESTRANK | V5_DEBG, "Warmup msg", rank);
            }
        }
    }
}

void Worker::createExpanderGraph() {

    // Pick fixed number k of bounce destinations
    int numBounceAlternatives = _params.numBounceAlternatives();
    int numWorkers = MyMpi::size(_comm);
    if (numWorkers == 1) return; // no hops

    // Check validity of num bounce alternatives
    if (2*numBounceAlternatives > numWorkers) {
        numBounceAlternatives = numWorkers / 2;
        log(V1_WARN, "[WARN] Num bounce alternatives must be at most half the number of workers!\n");
        log(V1_WARN, "[WARN] Falling back to safe value r=%i.\n", numBounceAlternatives);
    }  

    // Create graph, get outgoing edges from this node
    if (_params.maxIdleDistance() > 0) {
        _hop_destinations = AdjustablePermutation::createUndirectedExpanderGraph(numWorkers, numBounceAlternatives, _world_rank);        
    } else {
        auto permutations = AdjustablePermutation::getPermutations(numWorkers, numBounceAlternatives);
        _hop_destinations = AdjustablePermutation::createExpanderGraph(permutations, _world_rank);
        if (_params.hopsUntilCollectiveAssignment() >= 0)
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
    for (int dest : _hop_destinations) {
        _neighbor_idle_distance[dest] = 0; // initially, all workers are idle
    }

    // Output found bounce alternatives
    std::string info = "";
    for (size_t i = 0; i < _hop_destinations.size(); i++) {
        info += std::to_string(_hop_destinations[i]) + " ";
    }
    log(V3_VERB, "My bounce alternatives: %s\n", info.c_str());
    assert((int)_hop_destinations.size() == numBounceAlternatives);
}

void Worker::advance(float time) {

    if (time < 0) time = Timer::elapsedSeconds();

    // Reset watchdog
    _watchdog.reset(time);

    if (_was_busy != _job_db.isBusyOrCommitted()) {
        // Load status changed since last cycle
        sendStatusToNeighbors();
        _was_busy = !_was_busy;
    }
    
    if (_periodic_stats_check.ready()) {
        // Print stats

        // For this process and subprocesses
        if (_node_stats_calculated.load(std::memory_order_acquire)) {
            
            _sys_state.setLocal(SYSSTATE_GLOBALMEM, _node_memory_gbs);
            log(V4_VVER, "mem=%.2fGB mt_cpu=%.3f mt_sys=%.3f\n", _node_memory_gbs, _mainthread_cpu_share, _mainthread_sys_share);

            // Recompute stats for next query time
            // (concurrently because computation of PSS is expensive)
            _node_stats_calculated.store(false, std::memory_order_relaxed);
            auto pid = Proc::getPid();
            ProcessWideThreadPool::get().addTask([&, pid]() {
                auto memoryKbs = Proc::getRecursiveProportionalSetSizeKbs(Proc::getPid());
                auto memoryGbs = memoryKbs / 1024.f / 1024.f;
                _node_memory_gbs = memoryGbs;
                Proc::getThreadCpuRatio(Proc::getTid(), _mainthread_cpu_share, _mainthread_sys_share);
                _node_stats_calculated.store(true, std::memory_order_release);
            });
        }

        // Print further stats?
        if (_periodic_big_stats_check.ready()) {

            // For the current job
            if (_job_db.hasActiveJob()) {
                Job& job = _job_db.getActive();
                job.appl_dumpStats();
                if (job.getJobTree().isRoot()) {
                    std::string commStr = "";
                    for (size_t i = 0; i < job.getJobComm().size(); i++) {
                        commStr += " " + std::to_string(job.getJobComm()[i]);
                    }
                    if (!commStr.empty()) log(V4_VVER, "%s job comm:%s\n", job.toStr(), commStr.c_str());
                }
            }
        }
    }

    // Advance load balancing operations
    if (_periodic_balance_check.ready()) {
        _job_db.advanceBalancing();

        if (!_job_db.isBusyOrCommitted() && _time_only_idle_worker > 0 && time - _time_only_idle_worker >= 0.05) {
            // This worker is the only idle worker within its local vicinity since some time
            log(V4_VVER, "All neighbors are busy - requesting work\n");
            WorkRequest req(_world_rank, _job_db.getGlobalBalancingEpoch());
            MyMpi::isend(Random::choice(_hop_destinations), MSG_REQUEST_WORK, req);
            _time_only_idle_worker = -1;
        }

        // Advance collective assignment of nodes
        if (_params.hopsUntilCollectiveAssignment() >= 0) {
            _coll_assign.advance(_job_db.getGlobalBalancingEpoch());
        }
    }

    // Do diverse periodic maintenance tasks
    if (_periodic_maintenance.ready()) {
        
        // Forget jobs that are old or wasting memory
        _job_db.forgetOldJobs();

        // Continue to bounce requests which were deferred earlier
        for (auto& [req, senderRank] : _job_db.getDeferredRequestsToForward(time)) {
            bounceJobRequest(req, senderRank);
        }

        _bfs.collectGarbage(_job_db.getGlobalBalancingEpoch());
    }

    // Check active job
    if (_periodic_job_check.ready()) {
        
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
                    IntVec payload({job.getId(), job.getRevision(), result});
                    log(LOG_ADD_DESTRANK | V4_VVER, "%s : sending finished info", jobRootRank, job.toStr());
                    MyMpi::isend(jobRootRank, MSG_NOTIFY_RESULT_FOUND, payload);
                    job.setResultTransferPending(true);
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
                for (auto it = waitingRankRevPairs.begin(); it != waitingRankRevPairs.end(); ++it) {
                    auto& [rank, rev] = *it;
                    if (rev > job.getRevision()) continue;
                    if (job.getJobTree().hasLeftChild() && job.getJobTree().getLeftChildNodeRank() == rank) {
                        // Left child
                        sendRevisionDescription(id, rev, rank);
                    } else if (job.getJobTree().hasRightChild() && job.getJobTree().getRightChildNodeRank() == rank) {
                        // Right child
                        sendRevisionDescription(id, rev, rank);
                    } // else: obsolete request
                    // Remove processed request
                    it = waitingRankRevPairs.erase(it);
                    it--;
                }
            }

            // Job communication (e.g. clause sharing)
            if (job.wantsToCommunicate()) job.communicate();
        }

    }

    // Advance an all-reduction of the current system state
    if (_sys_state.aggregate(time)) {
        float* result = _sys_state.getGlobal();
        int verb = (_world_rank == 0 ? V2_INFO : V5_DEBG);

        int numDesires = result[SYSSTATE_NUMDESIRES];
        int numFulfilledDesires = result[SYSSTATE_NUMFULFILLEDDESIRES];
        float ratioFulfilled = numDesires <= 0 ? 0 : (float)numFulfilledDesires / numDesires;
        float latency = numFulfilledDesires <= 0 ? 0 : result[SYSSTATE_SUMDESIRELATENCIES] / numFulfilledDesires;

        log(verb, "sysstate busyratio=%.3f cmtdratio=%.3f jobs=%i globmem=%.2fGB newreqs=%i hops=%i\n", 
                    result[SYSSTATE_BUSYRATIO]/MyMpi::size(_comm), result[SYSSTATE_COMMITTEDRATIO]/MyMpi::size(_comm), 
                    (int)result[SYSSTATE_NUMJOBS], result[SYSSTATE_GLOBALMEM], (int)result[SYSSTATE_SPAWNEDREQUESTS], 
                    (int)result[SYSSTATE_NUMHOPS]);
        // Reset fields which are added to incrementally
        _sys_state.setLocal(SYSSTATE_NUMHOPS, 0);
        _sys_state.setLocal(SYSSTATE_SPAWNEDREQUESTS, 0);
        _sys_state.setLocal(SYSSTATE_NUMDESIRES, 0);
        _sys_state.setLocal(SYSSTATE_NUMFULFILLEDDESIRES, 0);
        _sys_state.setLocal(SYSSTATE_SUMDESIRELATENCIES, 0);
    }
}

void Worker::handleNotifyJobAborting(MessageHandle& handle) {

    int jobId = Serializable::get<int>(handle.getRecvData());
    if (!_job_db.has(jobId)) return;

    interruptJob(jobId, /*terminate=*/true, /*reckless=*/true);
    
    if (_job_db.get(jobId).getJobTree().isRoot()) {
        // Forward information on aborted job to client
        MyMpi::isend(_job_db.get(jobId).getJobTree().getParentNodeRank(), 
            MSG_NOTIFY_CLIENT_JOB_ABORTING, handle.moveRecvData());
    }
}

void Worker::handleAnswerAdoptionOffer(MessageHandle& handle) {

    IntPair pair = Serializable::get<IntPair>(handle.getRecvData());
    int jobId = pair.first;
    bool accepted = pair.second == 1;

    // Retrieve according job commitment
    if (!_job_db.hasCommitment(jobId)) {
        log(V1_WARN, "[WARN] Job commitment for #%i not present despite adoption accept msg\n", jobId);
        return;
    }
    const JobRequest& req = _job_db.getCommitment(jobId);
    assert(_job_db.has(jobId));
    Job &job = _job_db.get(jobId);

    if (accepted) {
        // Accepted
    
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
        log(LOG_ADD_SRCRANK | V4_VVER, "Rejected to become %s : uncommitting", handle.source, job.toStr());
        _job_db.uncommit(req.jobId);
    }
}

void Worker::handleQueryJobDescription(MessageHandle& handle) {
    IntPair pair = Serializable::get<IntPair>(handle.getRecvData());
    int jobId = pair.first;
    int revision = pair.second;

    assert(_job_db.has(jobId));
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
        || log_return_false("%i != %i\n", descPtr->size(), job.getDescription().getTransferSize(revision)));
    MyMpi::isend(dest, MSG_SEND_JOB_DESCRIPTION, descPtr);
    log(LOG_ADD_DESTRANK | V4_VVER, "Sent job desc. of %s rev. %i, size %i", dest, 
            job.toStr(), revision, descPtr->size());
}

void Worker::handleRejectOneshot(MessageHandle& handle) {
    OneshotJobRequestRejection rej = Serializable::get<OneshotJobRequestRejection>(handle.getRecvData());
    JobRequest& req = rej.request;
    log(LOG_ADD_SRCRANK | V5_DEBG, "%s rejected by dormant child", handle.source, 
            _job_db.toStr(req.jobId, req.requestedNodeIndex).c_str());

    if (_job_db.isAdoptionOfferObsolete(req)) return;

    Job& job = _job_db.get(req.jobId);
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
            log(LOG_ADD_DESTRANK | V4_VVER, "%s : query dormant child", rank, job.toStr());
            _sys_state.addLocal(SYSSTATE_SPAWNEDREQUESTS, 1);
        }
    }

    if (doNormalHopping) {
        log(V4_VVER, "%s : switch to normal hops\n", job.toStr());
        req.numHops = -1;
        bounceJobRequest(req, handle.source);
    }
}

void Worker::handleRequestNode(MessageHandle& handle, JobDatabase::JobRequestMode mode) {

    JobRequest req = Serializable::get<JobRequest>(handle.getRecvData());

    // Discard request if it has become obsolete
    if (_job_db.isRequestObsolete(req)) {
        log(LOG_ADD_SRCRANK | V3_VERB, "DISCARD %s mode=%i", handle.source, 
                req.toStr().c_str(), mode);
        if (_params.hopsUntilCollectiveAssignment() >= 0) _coll_assign.setStatusDirty();
        return;
    }

    if (mode == JobDatabase::NORMAL && _job_db.isBusyOrCommitted() && !_recent_work_requests.empty()) {
        // Forward the job request to the source of a recent work request

        // Remove old work requests and try and find a recent one
        auto it = _recent_work_requests.begin();
        while (it != _recent_work_requests.end()) {
            if (it->balancingEpoch+1 < _job_db.getGlobalBalancingEpoch()) {
                it = _recent_work_requests.erase(it);
            } else {
                break;
            }
        }
        // Found one?
        if (it != _recent_work_requests.end()) {
            WorkRequest wreq = *_recent_work_requests.begin();
            req.numHops++;
            _sys_state.addLocal(SYSSTATE_NUMHOPS, 1);
            log(LOG_ADD_DESTRANK | V4_VVER, "Forward %s to recent work req.", wreq.requestingRank, req.toStr().c_str());
            MyMpi::isend(wreq.requestingRank, MSG_REQUEST_NODE, req);
            _recent_work_requests.erase(_recent_work_requests.begin());
            return;
        }
    }

    int removedJob;
    auto adoptionResult = _job_db.tryAdopt(req, mode, handle.source, removedJob);
    if (adoptionResult == JobDatabase::ADOPT_FROM_IDLE || adoptionResult == JobDatabase::ADOPT_REPLACE_CURRENT) {

        if (adoptionResult == JobDatabase::ADOPT_REPLACE_CURRENT) {
            Job& job = _job_db.get(removedJob);
            IntPair pair(job.getId(), job.getIndex());
            MyMpi::isend(job.getJobTree().getParentNodeRank(), MSG_NOTIFY_NODE_LEAVING_JOB, pair);
        }

        // Adoption takes place
        std::string jobstr = _job_db.toStr(req.jobId, req.requestedNodeIndex);
        log(LOG_ADD_SRCRANK | V3_VERB, "ADOPT %s mode=%i", handle.source, req.toStr().c_str(), mode);
        assert(!_job_db.isBusyOrCommitted() || log_return_false("Adopting a job, but not idle!\n"));

        // Commit on the job, send a request to the parent
        if (!_job_db.has(req.jobId)) {
            // Job is not known yet: create instance
            _job_db.createJob(MyMpi::size(_comm), _world_rank, req.jobId, req.application);
        }
        _job_db.commit(req);
        MyMpi::isend(req.requestingNodeRank, 
            req.requestedNodeIndex == 0 ? MSG_OFFER_ADOPTION_OF_ROOT : MSG_OFFER_ADOPTION,
            req);

    } else if (adoptionResult == JobDatabase::REJECT) {
        if (req.requestedNodeIndex == 0 && _job_db.has(req.jobId) && _job_db.get(req.jobId).getJobTree().isRoot()) {
            // I have the dormant root of this request, but cannot adopt right now:
            // defer until I can (e.g., until a made commitment can be broken)
            log(V4_VVER, "Defer pending root reactivation %s\n", req.toStr().c_str());
            _job_db.setPendingRootReactivationRequest(std::move(req));
        } else if (mode == JobDatabase::TARGETED_REJOIN) {
            // Send explicit rejection message
            OneshotJobRequestRejection rej(req, _job_db.hasDormantJob(req.jobId));
            log(LOG_ADD_DESTRANK | V5_DEBG, "decline oneshot request for %s", handle.source, 
                        _job_db.toStr(req.jobId, req.requestedNodeIndex).c_str());
            MyMpi::isend(handle.source, MSG_REJECT_ONESHOT, rej);
        } else if (mode == JobDatabase::NORMAL) {
            // Continue job finding procedure somewhere else
            bounceJobRequest(req, handle.source);
        }
    }
}

void Worker::handleIncrementalJobFinished(MessageHandle& handle) {
    int jobId = Serializable::get<int>(handle.getRecvData());
    if (_job_db.has(jobId)) {
        log(V3_VERB, "Incremental job %s done\n", _job_db.get(jobId).toStr());
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
        log(V1_WARN, "[WARN] Job message from unknown job #%i\n", jobId);
        return;
    }
    // Give message to corresponding job
    Job& job = _job_db.get(jobId);
    if (job.getState() == ACTIVE) job.communicate(handle.source, msg);
}

void Worker::handleOfferAdoption(MessageHandle& handle) {

    JobRequest req = Serializable::get<JobRequest>(handle.getRecvData());
    log(LOG_ADD_SRCRANK | V4_VVER, "Adoption offer for %s", handle.source, 
                    _job_db.toStr(req.jobId, req.requestedNodeIndex).c_str());

    bool reject = false;
    if (!_job_db.has(req.jobId)) {
        reject = true;

    } else {
        // Retrieve concerned job
        Job &job = _job_db.get(req.jobId);

        // Check if node should be adopted or rejected
        if (_job_db.isAdoptionOfferObsolete(req)) {
            // Obsolete request
            log(LOG_ADD_SRCRANK | V3_VERB, "REJECT %s", handle.source, req.toStr().c_str());
            reject = true;

        } else {
            // Adopt the job.
            // Child will start / resume its job solvers.
            // Mark new node as one of the node's children
            auto relative = job.getJobTree().setChild(handle.source, req.requestedNodeIndex);
            if (relative == JobTree::TreeRelative::NONE) assert(req.requestedNodeIndex == 0);
        }
    }

    MyMpi::isend(handle.source, MSG_ANSWER_ADOPTION_OFFER, IntPair(req.jobId, reject ? 0 : 1));
}

void Worker::handleQueryJobResult(MessageHandle& handle) {

    // Receive acknowledgement that the client received the advertised result size
    // and wishes to receive the full job result
    int jobId = Serializable::get<int>(handle.getRecvData());
    assert(_job_db.has(jobId));
    const JobResult& result = _job_db.get(jobId).getResult();
    log(LOG_ADD_DESTRANK | V3_VERB, "Send result of #%i rev. %i to client", handle.source, jobId, result.revision);
    MyMpi::isend(handle.source, MSG_SEND_JOB_RESULT, result);
    _job_db.get(jobId).setResultTransferPending(false);
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
    log(LOG_ADD_DESTRANK | V4_VVER, "Answer #%i volume query with v=%i", handle.source, jobId, volume);
    MyMpi::isend(handle.source, MSG_NOTIFY_VOLUME_UPDATE, response);
}

void Worker::handleNotifyResultObsolete(MessageHandle& handle) {
    IntVec res = Serializable::get<IntVec>(handle.getRecvData());
    int jobId = res[0];
    //int revision = res[1];
    if (!_job_db.has(jobId)) return;
    log(LOG_ADD_SRCRANK | V4_VVER, "job result for %s unwanted", handle.source, _job_db.get(jobId).toStr());
    _job_db.get(jobId).setResultTransferPending(false);
}

void Worker::handleSendJobDescription(MessageHandle& handle) {
    const auto& data = handle.getRecvData();
    int jobId = data.size() >= sizeof(int) ? Serializable::get<int>(data) : -1;
    log(LOG_ADD_SRCRANK | V4_VVER, "Got desc. of size %i for job #%i", handle.source, data.size(), jobId);
    if (jobId == -1 || !_job_db.has(jobId)) {
        if (_job_db.hasCommitment(jobId)) _job_db.uncommit(jobId);
        return;
    }

    // Append revision description to job
    auto& job = _job_db.get(jobId);
    auto dataPtr = std::shared_ptr<std::vector<uint8_t>>(
        new std::vector<uint8_t>(handle.moveRecvData())
    );
    bool valid = _job_db.appendRevision(jobId, dataPtr, handle.source);
    if (!valid) return;

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
        log(V1_WARN, "[WARN] Volume update for unknown #%i\n", jobId);
        return;
    }

    // Update volume assignment in job instance (and its children)
    updateVolume(jobId, volume, balancingEpoch, 0);
}

void Worker::handleNotifyNodeLeavingJob(MessageHandle& handle) {

    // Retrieve job
    IntPair recv = Serializable::get<IntPair>(handle.getRecvData());
    int jobId = recv.first;
    int index = recv.second;
    if (!_job_db.has(jobId)) return;
    Job& job = _job_db.get(jobId);

    // Prune away the respective child if necessary
    auto pruned = job.getJobTree().prune(handle.source, index);

    // If necessary, find replacement
    if (pruned != JobTree::TreeRelative::NONE && index < job.getVolume()) {

        // Try to find a dormant child that is not the message source
        int tag = MSG_REQUEST_NODE_ONESHOT;
        int nextNodeRank = job.getJobTree().getRankOfNextDormantChild();
        if (nextNodeRank == -1 || nextNodeRank == handle.source) {
            // If unsucessful, pick a random node
            tag = MSG_REQUEST_NODE;
            nextNodeRank = _params.derandomize() ? getWeightedRandomNeighbor() : getRandomNonSelfWorkerNode();
        }
        
        // Initiate search for a replacement for the defected child
        JobRequest req = job.getJobTree().getJobRequestFor(jobId, pruned, _job_db.getGlobalBalancingEpoch(), 
                job.getDescription().getApplication());
        log(LOG_ADD_DESTRANK | V4_VVER, "%s : request replacement for %s", nextNodeRank, 
                        job.toStr(), _job_db.toStr(jobId, index).c_str());
        MyMpi::isend(nextNodeRank, tag, req);
        _sys_state.addLocal(SYSSTATE_SPAWNEDREQUESTS, 1);
        if (pruned == JobTree::LEFT_CHILD) job.getJobTree().setDesireLeft(Timer::elapsedSeconds());
        else job.getJobTree().setDesireRight(Timer::elapsedSeconds());
    }

    // Initiate communication if the job now became willing to communicate
    if (job.wantsToCommunicate()) job.communicate();
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
        log(V1_WARN, "[WARN] Invalid adressee for job result of #%i\n", jobId);
    } else if (_job_db.get(jobId).getRevision() > revision || _job_db.get(jobId).isRevisionSolved(revision)) {
        obsolete = true;
        log(LOG_ADD_SRCRANK | V4_VVER, "Discard obsolete result for job #%i rev. %i", handle.source, jobId, revision);
    }
    if (obsolete) {
        MyMpi::isendCopy(handle.source, MSG_NOTIFY_RESULT_OBSOLETE, handle.getRecvData());
        return;
    }
    
    log(LOG_ADD_SRCRANK | V3_VERB, "#%i rev. %i solved", handle.source, jobId, revision);
    _job_db.get(jobId).setRevisionSolved(revision);

    // Terminate job and propagate termination message
    if (_job_db.get(jobId).getDescription().isIncremental()) {
        handleInterrupt(handle);
    } else {
        handleNotifyJobTerminating(handle);
    }

    // Notify client
    sendJobDoneWithStatsToClient(jobId, handle.source);
}

void Worker::handleNotifyNeighborStatus(MessageHandle& handle) {
    bool isBusy = handle.getRecvData().at(0) == 1;
    updateNeighborStatus(handle.source, isBusy);
}

void Worker::handleNotifyNeighborIdleDistance(MessageHandle& handle) {
    int distBefore = getIdleDistance();
    _neighbor_idle_distance[handle.source] = Serializable::get<int>(handle.getRecvData());
    int distAfter = getIdleDistance();
    if (distAfter != distBefore) {
        log(V4_VVER, "New idle distance %i\n", distAfter);
        sendStatusToNeighbors();
    }
}

int Worker::getIdleDistance() {
    if (!_job_db.isBusyOrCommitted()) return 0;
    int minDistance = _params.maxIdleDistance();
    for (const auto& [rank, dist] : _neighbor_idle_distance) {
        minDistance = std::min(minDistance, dist+1);
    }
    return minDistance;
}

void Worker::updateNeighborStatus(int rank, bool busy) {
    if (busy) _busy_neighbors.insert(rank);
    else _busy_neighbors.erase(rank);
    if (!_job_db.isBusyOrCommitted() && _busy_neighbors.size() == _hop_destinations.size()) {
        if (_time_only_idle_worker <= 0)
            _time_only_idle_worker = Timer::elapsedSeconds();
    } else {
        _time_only_idle_worker = -1;
    }
}

void Worker::handleRequestWork(MessageHandle& handle) {
    WorkRequest req = Serializable::get<WorkRequest>(handle.getRecvData());
    
    // Is this work request obsolete?
    // - I am this worker but I am not idle any more
    if (req.requestingRank == _world_rank && _job_db.isBusyOrCommitted()) return;
    // - _busy_neighbors contains this worker
    if (_busy_neighbors.count(req.requestingRank)) return;
    // - # hops exceeding significant ratio of # workers
    if (req.numHops >= std::ceil(0.1 * MyMpi::size(_comm))) return;
    
    // Remember work request for upcoming job requests
    // if the request does not originate from myself
    if (req.requestingRank != _world_rank) {
        // Try to find a recent work request from the same rank
        auto foundReq = _recent_work_requests.end();
        for (auto it = _recent_work_requests.begin(); it != _recent_work_requests.end(); ++it) {
            if (it->requestingRank == handle.source) {
                foundReq = it;
                break;
            }
        }
        // Found? -> Remove it if the new request dominates the old one
        bool dominatingOrNovel = true;
        if (foundReq != _recent_work_requests.end()) {
            const WorkRequest& otherReq = *foundReq;
            dominatingOrNovel = req.balancingEpoch > otherReq.balancingEpoch || req.numHops < otherReq.numHops;
            if (dominatingOrNovel) _recent_work_requests.erase(foundReq);
        }
        // Insert new request if dominating or novel
        if (dominatingOrNovel) _recent_work_requests.insert(req);
        // Cap number of resident work requests
        if (_recent_work_requests.size() > 3)
            _recent_work_requests.erase(std::prev(_recent_work_requests.end()));
    }

    // Past-past balancing epoch: remember request but do not bounce it any further
    if (req.balancingEpoch+1 < _job_db.getGlobalBalancingEpoch()) return;

    // Bounce work request
    req.numHops++;
    int dest = Random::choice(_hop_destinations);
    // (if possible, not just back to the sender)
    while (_hop_destinations.size() > 1 && dest == handle.source) {
        dest = Random::choice(_hop_destinations);
    }
    MyMpi::isend(dest, MSG_REQUEST_WORK, req);
}

void Worker::bounceJobRequest(JobRequest& request, int senderRank) {

    // Increment #hops
    request.numHops++;
    int num = request.numHops;
    _sys_state.addLocal(SYSSTATE_NUMHOPS, 1);

    // Show warning if #hops is a large power of two
    if ((num >= 512) && ((num & (num - 1)) == 0)) {
        log(V1_WARN, "[WARN] %s\n", request.toStr().c_str());
    }

    if (num >= _params.hopsUntilBfs() && num % _params.hopsBetweenBfs() == 0 && _params.maxBfsDepth() > 0) {
        // Initiate a BFS to find an idle worker for this request
        // Increase depth for each further BFS for this request
        int depth = 1 + (num - _params.hopsUntilBfs()) / _params.hopsBetweenBfs();
        depth = std::min(depth, _params.maxBfsDepth());
        log(V4_VVER, "Initiate BFS (d=%i) for %s\n", depth, request.toStr().c_str());
        _bfs.startSearch(request, depth);
        return;
    }

    if (_params.hopsUntilCollectiveAssignment() >= 0 && num >= _params.hopsUntilCollectiveAssignment()
        && request.requestedNodeIndex > 0) {
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
    log(LOG_ADD_DESTRANK | V5_DEBG, "Hop %s", nextRank, _job_db.toStr(request.jobId, request.requestedNodeIndex).c_str());
    MyMpi::isend(nextRank, MSG_REQUEST_NODE, request);
}

void Worker::initiateVolumeUpdate(int jobId) {
    auto& job = _job_db.get(jobId);
    if (_params.explicitVolumeUpdates()) {
        if (job.getJobTree().isRoot()) {
            // Root worker: update volume (to trigger growth if desired)
            if (job.getVolume() > 1) updateVolume(jobId, job.getVolume(), _job_db.getGlobalBalancingEpoch(), 0);
        } else {
            // Non-root worker: query parent for the volume of this job
            IntVec payload({jobId});
            MyMpi::isend(job.getJobTree().getParentNodeRank(), MSG_QUERY_VOLUME, payload);
        }
    } else {
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

    if (!_job_db.has(jobId)) return;
    Job &job = _job_db.get(jobId);

    if (job.getState() != ACTIVE) {
        // Job is not active right now

        // If I am committed with the job and the job is shrinking accordingly, uncommit
        if (job.hasCommitment() && job.getIndex() > 0 && job.getIndex() >= volume) {
            log(V4_VVER, "%s shrunk : uncommitting\n", job.toStr());
            _job_db.uncommit(jobId);
            MyMpi::isend(job.getJobTree().getParentNodeRank(), MSG_NOTIFY_NODE_LEAVING_JOB, IntPair(jobId, job.getIndex()));
        }
        return;
    }

    int thisIndex = job.getIndex();
    log(job.getVolume() == volume || thisIndex > 0 ? V4_VVER : V3_VERB, "%s : update v=%i epoch=%i lastreqsepoch=%i evlat=%.5f\n", 
        job.toStr(), volume, balancingEpoch, job.getJobTree().getBalancingEpochOfLastRequests(), eventLatency);
    job.updateVolumeAndUsedCpu(volume);

    if (job.getJobTree().isRoot() && job.getJobTree().getBalancingEpochOfLastRequests() == -1) {
        // Job's volume is updated for the first time since its activation
        job.setTimeOfFirstVolumeUpdate(Timer::elapsedSeconds());
    }

    // Prepare volume update to propagate down the job tree
    IntVec payload{jobId, volume, balancingEpoch};

    // Mono instance mode: Set job tree permutation to identity
    bool mono = _params.monoFilename.isSet();

    // For each potential child (left, right):
    bool has[2] = {job.getJobTree().hasLeftChild(), job.getJobTree().hasRightChild()};
    int indices[2] = {job.getJobTree().getLeftChildIndex(), job.getJobTree().getRightChildIndex()};
    int ranks[2] = {-1, -1};
    for (int i = 0; i < 2; i++) {
        int nextIndex = indices[i];
        if (has[i]) {
            if (_params.explicitVolumeUpdates()) {
                // Propagate volume update
                ranks[i] = i == 0 ? job.getJobTree().getLeftChildNodeRank() : job.getJobTree().getRightChildNodeRank();
                MyMpi::isend(ranks[i], MSG_NOTIFY_VOLUME_UPDATE, payload);
            }
        } else if (nextIndex < volume 
                && job.getJobTree().getBalancingEpochOfLastRequests() < balancingEpoch) {
            if (_job_db.hasDormantRoot()) {
                // Becoming an inner node is not acceptable
                // because then the dormant root cannot be restarted seamlessly
                log(V4_VVER, "%s cannot grow due to dormant root\n", job.toStr());
                _job_db.suspend(jobId);
                MyMpi::isend(job.getJobTree().getParentNodeRank(), 
                    MSG_NOTIFY_NODE_LEAVING_JOB, IntPair(jobId, thisIndex));
                break;
            }
            // Grow
            log(V5_DEBG, "%s : grow\n", job.toStr());
            if (mono) job.getJobTree().updateJobNode(indices[i], indices[i]);
            JobRequest req(jobId, job.getDescription().getApplication(), job.getJobTree().getRootNodeRank(), 
                    _world_rank, nextIndex, Timer::elapsedSeconds(), balancingEpoch, 0);
            req.revision = job.getDesiredRevision();
            int tag = MSG_REQUEST_NODE_ONESHOT;
            int nextNodeRank = job.getJobTree().getRankOfNextDormantChild(); 
            if (nextNodeRank < 0) {
                tag = MSG_REQUEST_NODE;
                ranks[i] = i == 0 ? job.getJobTree().getLeftChildNodeRank() : job.getJobTree().getRightChildNodeRank();
                nextNodeRank = ranks[i];
            }
            log(LOG_ADD_DESTRANK | V3_VERB, "%s growing: %s", nextNodeRank, 
                        job.toStr(), req.toStr().c_str());
            MyMpi::isend(nextNodeRank, tag, req);
            _sys_state.addLocal(SYSSTATE_SPAWNEDREQUESTS, 1);
            if (i == 0) job.getJobTree().setDesireLeft(Timer::elapsedSeconds());
            else job.getJobTree().setDesireRight(Timer::elapsedSeconds());
        } else {
            // Job does not want to grow - any more (?) - so unset any previous desire
            if (i == 0) job.getJobTree().unsetDesireLeft();
            else job.getJobTree().unsetDesireRight();
        }
    }
    job.getJobTree().setBalancingEpochOfLastRequests(balancingEpoch);

    // Shrink (and pause solving) if necessary
    if (thisIndex > 0 && thisIndex >= volume) {
        log(V3_VERB, "%s shrinking\n", job.toStr());
        _job_db.suspend(jobId);
        MyMpi::isend(job.getJobTree().getParentNodeRank(), MSG_NOTIFY_NODE_LEAVING_JOB, IntPair(jobId, thisIndex));
    }
}

void Worker::interruptJob(int jobId, bool terminate, bool reckless) {

    if (!_job_db.has(jobId)) return;
    Job& job = _job_db.get(jobId);

    // Ignore if this job node is already in the goal state
    // (also implying that it already forwarded such a request downwards)
    if (!terminate && job.getState() == SUSPENDED) return;

    // Propagate message down the job tree
    int msgTag;
    if (terminate && reckless) msgTag = MSG_NOTIFY_JOB_ABORTING;
    else if (terminate) msgTag = MSG_NOTIFY_JOB_TERMINATING;
    else msgTag = MSG_INTERRUPT;
    if (job.getJobTree().hasLeftChild()) {
        MyMpi::isend(job.getJobTree().getLeftChildNodeRank(), msgTag, IntVec({jobId}));
        log(LOG_ADD_DESTRANK | V4_VVER, "Propagate interruption of %s ...", job.getJobTree().getLeftChildNodeRank(), job.toStr());
    }
    if (job.getJobTree().hasRightChild()) {
        MyMpi::isend(job.getJobTree().getRightChildNodeRank(), msgTag, IntVec({jobId}));
        log(LOG_ADD_DESTRANK | V4_VVER, "Propagate interruption of %s ...", job.getJobTree().getRightChildNodeRank(), job.toStr());
    }
    for (auto childRank : job.getJobTree().getPastChildren()) {
        MyMpi::isend(childRank, msgTag, IntVec({jobId}));
        log(LOG_ADD_DESTRANK | V4_VVER, "Propagate interruption of %s (past child) ...", childRank, job.toStr());
    }
    if (terminate) job.getJobTree().getPastChildren().clear();

    // Suspend or terminate the job
    if (terminate) _job_db.terminate(jobId);
    else if (job.getState() == ACTIVE) _job_db.suspend(jobId);
}

void Worker::sendJobDoneWithStatsToClient(int jobId, int successfulRank) {
    auto& job = _job_db.get(jobId);
    const JobResult& result = job.getResult();

    int clientRank = job.getDescription().getClientRank();
    log(LOG_ADD_DESTRANK | V4_VVER, "%s : inform client job is done", clientRank, job.toStr());
    job.updateVolumeAndUsedCpu(job.getVolume());
    JobStatistics stats;
    stats.jobId = jobId;
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

void Worker::sendStatusToNeighbors() {
    if (_params.workRequests()) {
        std::vector<uint8_t> payload(1, _job_db.isBusyOrCommitted() ? 1 : 0);
        for (int rank : _hop_destinations) {
            MyMpi::isendCopy(rank, MSG_NOTIFY_NEIGHBOR_STATUS, payload);
        }
    }
    if (_params.maxIdleDistance() > 0) {
        int idleDistance = getIdleDistance();
        auto payload = IntVec({idleDistance});
        for (int rank : _hop_destinations) {
            MyMpi::isend(rank, MSG_NOTIFY_NEIGHBOR_IDLE_DISTANCE, payload);
        }
    }
}

int Worker::getWeightedRandomNeighbor() {
    
    int sum = 0;
    for (const auto& [rank, dist] : _neighbor_idle_distance) 
        sum += 1 + _params.maxIdleDistance() - dist;
    
    int rand = (int) (sum*Random::rand());
    
    int newSum = 0;
    for (const auto& [rank, dist] : _neighbor_idle_distance) {
        newSum += 1 + _params.maxIdleDistance() - dist;
        if (rand < newSum) return rank;
    }
    abort();
}

bool Worker::checkTerminate(float time) {
    bool terminate = false;
    if (Terminator::isTerminating(/*fromMainThread=*/true)) terminate = true;
    if (_global_timeout > 0 && time > _global_timeout) {
        terminate = true;
    }
    if (terminate) {
        log(_world_rank == 0 ? V2_INFO : V3_VERB, "Terminating.\n");
        Terminator::setTerminating();
        return true;
    }
    return false;
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

    log(V4_VVER, "Destruct worker\n");

    if (_params.monoFilename.isSet() && _params.applicationSpawnMode() != "fork") {
        // Terminate directly without destructing resident job
        MPI_Finalize();
        Process::doExit(0);
    }
}