
#include "scheduling_manager.hpp"

#include "util/assert.hpp"
#include <algorithm>
#include <queue>
#include <utility>
#include <climits>

#include "util/sys/timer.hpp"
#include "util/logger.hpp"
#include "balancing/event_driven_balancer.hpp"
#include "util/sys/watchdog.hpp"
#include "util/sys/proc.hpp"
#include "util/sys/thread_pool.hpp"

SchedulingManager::SchedulingManager(Parameters& params, MPI_Comm& comm, 
            RandomizedRoutingTree& routingTree,
            JobRegistry& jobRegistry, WorkerSysState& sysstate):
        _params(params), _comm(comm), _routing_tree(routingTree),
        _req_cache(
            // Callback for deflecting a job request to another destination
            [&](JobRequest& req, int source) {deflectJobRequest(req, source);}
        ),
        _sys_state(sysstate), _job_registry(jobRegistry),
        _reactivation_scheduler(_job_registry,
            // Callback for emitting a job request
            [&](const JobRequest& req, int tag, bool left, int dest) {
                emitJobRequest(req, tag, left, dest);
            },
            // Callback for retrieving a job for a given ID
            [&](int jobId) {return has(jobId) ? &get(jobId) : nullptr;}
        ) {

    _wcsecs_per_instance = params.jobWallclockLimit();
    _cpusecs_per_instance = params.jobCpuLimit();
    _load_factor = params.loadFactor();
    assert(0 < _load_factor && _load_factor <= 1.0);
    _balance_period = params.balancingPeriod();       

    // Initialize balancer
    _balancer = std::unique_ptr<EventDrivenBalancer>(new EventDrivenBalancer(_comm, _params));
    _balancer->setVolumeUpdateCallback([&](int jobId, int volume, float eventLatency) {
        updateVolume(jobId, volume, getGlobalBalancingEpoch(), eventLatency);
    });
    _balancer->setBalancingDoneCallback([&]() {
        // apply any job requests which have arrived from a "future epoch"
        // which has now become the present (or a past) epoch
        for (auto& h : _req_cache.getArrivedFutureRequests(_balancer->getGlobalEpoch())) {
            LOG_ADD_SRC(V4_VVER, "From the future: tag=%i", h.source, h.tag);
            handleIncomingJobRequest(h, h.tag == MSG_REQUEST_NODE ? 
                SchedulingManager::JobRequestMode::NORMAL : 
                SchedulingManager::JobRequestMode::TARGETED_REJOIN);
        }
    });
    // Balancer message handling
    auto balanceCb = [&](auto& h) {handleBalancingMessage(h);};
    _subscriptions.emplace_back(MSG_COLLECTIVE_OPERATION, balanceCb);
    _subscriptions.emplace_back(MSG_REDUCE_DATA, balanceCb);
    _subscriptions.emplace_back(MSG_BROADCAST_DATA, balanceCb);

    MyMpi::getMessageQueue().registerSentCallback(MSG_SEND_JOB_DESCRIPTION, [&](int sendId) {
        handleJobDescriptionSent(sendId);
    });

    _subscriptions.emplace_back(MSG_ANSWER_ADOPTION_OFFER,
        [&](auto& h) {handleAnswerToAdoptionOffer(h);});
    _subscriptions.emplace_back(MSG_NOTIFY_JOB_ABORTING, 
        [&](auto& h) {handleJobInterruption(h);});
    _subscriptions.emplace_back(MSG_NOTIFY_JOB_TERMINATING, 
        [&](auto& h) {
            interruptJob(Serializable::get<int>(h.getRecvData()), /*terminate=*/true, /*reckless=*/false);
        }
    );
    _subscriptions.emplace_back(MSG_NOTIFY_RESULT_FOUND, 
        [&](auto& h) {handleJobResultFound(h);});
    _subscriptions.emplace_back(MSG_INCREMENTAL_JOB_FINISHED,
        [&](auto& h) {handleIncrementalJobFinished(h);});
    _subscriptions.emplace_back(MSG_INTERRUPT,
        [&](auto& h) {
            interruptJob(Serializable::get<int>(h.getRecvData()), /*terminate=*/false, /*reckless=*/false);
        });
    _subscriptions.emplace_back(MSG_NOTIFY_NODE_LEAVING_JOB, 
        [&](auto& h) {handleLeavingChild(h);});
    _subscriptions.emplace_back(MSG_NOTIFY_RESULT_OBSOLETE, 
        [&](auto& h) {handleObsoleteJobResult(h);});
    _subscriptions.emplace_back(MSG_NOTIFY_VOLUME_UPDATE, 
        [&](auto& h) {handleExplicitVolumeUpdate(h);});
    _subscriptions.emplace_back(MSG_OFFER_ADOPTION, 
        [&](auto& h) {handleAdoptionOffer(h);});
    _subscriptions.emplace_back(MSG_QUERY_JOB_DESCRIPTION,
        [&](auto& h) {handleQueryForJobDescription(h);});
    _subscriptions.emplace_back(MSG_QUERY_JOB_RESULT, 
        [&](auto& h) {handleQueryForJobResult(h);});
    _subscriptions.emplace_back(MSG_QUERY_VOLUME, 
        [&](auto& h) {handleQueryForExplicitVolumeUpdate(h);});
    _subscriptions.emplace_back(MSG_REJECT_ONESHOT, 
        [&](auto& h) {handleRejectionOfDirectedRequest(h);});
    _subscriptions.emplace_back(MSG_REQUEST_NODE, 
        [&](auto& h) {handleIncomingJobRequest(h, SchedulingManager::JobRequestMode::NORMAL);});
    _subscriptions.emplace_back(MSG_REQUEST_NODE_ONESHOT, 
        [&](auto& h) {handleIncomingJobRequest(h, SchedulingManager::JobRequestMode::TARGETED_REJOIN);});
    _subscriptions.emplace_back(MSG_SEND_APPLICATION_MESSAGE, 
        [&](auto& h) {handleApplicationMessage(h);});
    _subscriptions.emplace_back(MSG_JOB_TREE_REDUCTION, 
        [&](auto& h) {handleApplicationMessage(h);});
    _subscriptions.emplace_back(MSG_JOB_TREE_BROADCAST, 
        [&](auto& h) {handleApplicationMessage(h);});
    _subscriptions.emplace_back(MSG_SEND_JOB_DESCRIPTION, 
        [&](auto& h) {handleIncomingJobDescription(h);});
    _subscriptions.emplace_back(MSG_NOTIFY_ASSIGNMENT_UPDATE, 
        [&](auto& h) {_req_matcher->handle(h);});
    _subscriptions.emplace_back(MSG_SCHED_RELEASE_FROM_WAITING, 
        [&](auto& h) {handleJobReleasedFromWaitingForReactivation(h);});
    
    // Local scheduler message handling
    auto localSchedulerCb = [&](MessageHandle& handle) {
        _reactivation_scheduler.handle(handle);
    };
    _subscriptions.emplace_back(MSG_SCHED_INITIALIZE_CHILD_WITH_NODES, localSchedulerCb);
    _subscriptions.emplace_back(MSG_SCHED_RETURN_NODES, localSchedulerCb);
}

bool SchedulingManager::appendRevision(int jobId, const std::shared_ptr<std::vector<uint8_t>>& description, int source) {

    if (!has(jobId)) {
        LOG(V1_WARN, "[WARN] Unknown job #%i : discard desc. of size %i\n", jobId, description->size());
        return false;
    }
    auto& job = get(jobId);
    int rev = JobDescription::readRevisionIndex(*description);
    if (job.hasDescription()) {
        if (rev != job.getMaxConsecutiveRevision()+1) {
            // Revision data would cause a "hole" in the list of job revision data
            LOG(V1_WARN, "[WARN] #%i rev. %i inconsistent w/ max. consecutive rev. %i : discard desc. of size %i\n", 
                jobId, rev, job.getMaxConsecutiveRevision(), description->size());
            return false;
        }
    } else if (rev != 0) {
        LOG(V1_WARN, "[WARN] #%i invalid \"first\" rev. %i : discard desc. of size %i\n", jobId, rev, description->size());
            return false;
    }

    // Push revision description
    job.pushRevision(description);
    return true;
}

void SchedulingManager::execute(int jobId, int source) {

    if (!has(jobId)) {
        LOG(V1_WARN, "[WARN] Unknown job #%i\n", jobId);
        return;
    }
    auto& job = get(jobId);

    // Execute job
    setLoad(1, jobId);
    LOG_ADD_SRC(V3_VERB, "EXECUTE %s", source, job.toStr());
    if (job.getState() == INACTIVE) {
        // Execute job for the first time
        job.start();
    } else {
        // Restart job
        job.resume();
    }

    int demand = job.getDemand();
    _balancer->onActivate(job, demand);
    job.setLastDemand(demand);
}

void SchedulingManager::preregisterJobInBalancer(int jobId) {
    assert(has(jobId));
    auto& job = get(jobId);
    int demand = std::max(1, job.getJobTree().isRoot() ? 0 : job.getDemand());
    _balancer->onActivate(job, demand);
    if (job.getJobTree().isRoot()) job.setLastDemand(demand);
}

void SchedulingManager::checkActiveJob() {

    Job &job = _job_registry.getActive();
    int id = job.getId();
    bool isRoot = job.getJobTree().isRoot();

    bool abort = false;
    if (isRoot) abort = checkComputationLimits(id);
    if (abort) {
        // Timeout (CPUh or wallclock time) hit
        
        // "Virtual self message" aborting the job
        IntVec payload({id});
        MessageHandle handle;
        handle.tag = MSG_NOTIFY_JOB_ABORTING;
        handle.finished = true;
        handle.receiveSelfMessage(payload.serialize(), MyMpi::rank(MPI_COMM_WORLD));
        handleJobInterruption(handle);
        if (_params.monoFilename.isSet()) {
            // Single job hit a limit, so there is no solution to be reported:
            // begin to propagate exit signal
            MyMpi::isend(0, MSG_DO_EXIT, IntVec({0}));
        }

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
                payload = IntVec({id, resultStruct.revision, result});
                LOG_ADD_DEST(V4_VVER, "%s rev. %i: sending finished info", jobRootRank, job.toStr(), resultStruct.revision);
                _result_store.store(id, resultStruct.revision, std::move(resultStruct));
            }
            MyMpi::isend(jobRootRank, MSG_NOTIFY_RESULT_FOUND, payload);
        }

        // Update demand as necessary
        if (isRoot) {
            int demand = job.getDemand();
            if (demand != job.getLastDemand()) {
                // Demand updated
                handleDemandUpdate(job, demand);
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
                sendJobDescription(id, rev, rank);
            } else if (job.getJobTree().hasRightChild() && job.getJobTree().getRightChildNodeRank() == rank) {
                // Right child
                sendJobDescription(id, rev, rank);
            } // else: obsolete request
            // Remove processed request
            it = waitingRankRevPairs.erase(it);
        }
    }

    // Job communication (e.g. clause sharing)
    job.communicate();
}

bool SchedulingManager::checkComputationLimits(int jobId) {

    auto& job = get(jobId);
    if (!job.getJobTree().isRoot()) return false;
    return job.checkResourceLimit(_wcsecs_per_instance, _cpusecs_per_instance);
}

void SchedulingManager::handleIncomingJobRequest(MessageHandle& handle, JobRequestMode mode) {

    JobRequest req = Serializable::get<JobRequest>(handle.getRecvData());
    int source = handle.source;

    // Discard request if it has become obsolete
    if (isRequestObsolete(req)) {
        LOG_ADD_SRC(V3_VERB, "DISCARD %s mode=%i", source, 
                req.toStr().c_str(), mode);
        if (_params.hopsUntilCollectiveAssignment() >= 0) _req_matcher->setStatusDirty();
        return;
    }

    // Root request for the first revision of a new job?
    if (req.requestedNodeIndex == 0 && req.numHops == 0 && req.revision == 0) {
        // Probe balancer for a free spot.
        _balancer->onProbe(req.jobId);
        _req_cache.addRootRequest(std::move(req));
        return;
    }

    if (req.balancingEpoch > getGlobalBalancingEpoch()) {
        // Job request is "from the future": defer it until it is from the present
        LOG(V4_VVER, "Defer future req. %s\n", req.toStr().c_str());
        _req_cache.addFutureRequestMessage(req.balancingEpoch, std::move(handle));
        return;
    }

    // Explicit rejoin request from reactivation-based scheduling?
    if (_params.reactivationScheduling() && mode == SchedulingManager::TARGETED_REJOIN) {
        // Mark job as having been notified of the current scheduling and that it is not further needed.
        if (has(req.jobId)) get(req.jobId).getJobTree().stopWaitingForReactivation(req.balancingEpoch);
        if (_params.hopsUntilCollectiveAssignment() >= 0) _req_matcher->setStatusDirty();
    }

    // Decide whether to adopt the job.
    SchedulingManager::AdoptionResult adoptionResult = SchedulingManager::ADOPT_FROM_IDLE;
    int removedJob;
    if (_params.reactivationScheduling() && mode != SchedulingManager::TARGETED_REJOIN 
            && _job_registry.hasInactiveJobsWaitingForReactivation() && req.requestedNodeIndex > 0) {
        // In reactivation-based scheduling, block incoming requests if you are still waiting
        // for a notification from some job of which you have an inactive job node.
        // Does not apply for targeted requests!
        adoptionResult = SchedulingManager::REJECT;
    } else {
        adoptionResult = tryAdopt(req, mode, source, removedJob);
    }

    // Do I adopt the job?
    if (adoptionResult == SchedulingManager::ADOPT_FROM_IDLE || adoptionResult == SchedulingManager::ADOPT_REPLACE_CURRENT) {

        // Replaced the current job
        if (adoptionResult == SchedulingManager::ADOPT_REPLACE_CURRENT) {
            Job& job = get(removedJob);
            MyMpi::isend(job.getJobTree().getParentNodeRank(), MSG_NOTIFY_NODE_LEAVING_JOB, 
                IntVec({job.getId(), job.getIndex(), job.getJobTree().getRootNodeRank()}));
        }

        // Adoption takes place
        std::string jobstr = toStr(req.jobId, req.requestedNodeIndex);
        LOG_ADD_SRC(V3_VERB, "ADOPT %s mode=%i", source, req.toStr().c_str(), mode);
        assert(!_job_registry.isBusyOrCommitted() || LOG_RETURN_FALSE("Adopting a job, but not idle!\n"));

        // Commit on the job, send a request to the parent
        if (!has(req.jobId)) {
            // Job is not known yet: create instance
            Job& job = _job_registry.create(req.jobId, req.applicationId, req.incremental);
        }
        commit(req);
        if (_params.reactivationScheduling()) {
            _reactivation_scheduler.initializeReactivator(req, get(req.jobId));
        }
        MyMpi::isend(req.requestingNodeRank, 
            req.requestedNodeIndex == 0 ? MSG_OFFER_ADOPTION_OF_ROOT : MSG_OFFER_ADOPTION,
            req);

    } else if (adoptionResult == SchedulingManager::REJECT) {
        
        // Job request was rejected
        if (req.requestedNodeIndex == 0 && has(req.jobId) && get(req.jobId).getJobTree().isRoot()) {
            // I have the dormant root of this request, but cannot adopt right now:
            // defer until I can (e.g., until a made commitment can be broken)
            LOG(V4_VVER, "Defer pending root reactivation %s\n", req.toStr().c_str());
            _req_cache.setPendingRootReactivationRequest(std::move(req));
        } else if (mode == SchedulingManager::TARGETED_REJOIN) {
            // Send explicit rejection message
            OneshotJobRequestRejection rej(req, _job_registry.hasDormantJob(req.jobId));
            LOG_ADD_DEST(V5_DEBG, "REJECT %s myepoch=%i", source, 
                        req.toStr().c_str(), getGlobalBalancingEpoch());
            MyMpi::isend(source, MSG_REJECT_ONESHOT, rej);
        } else if (mode == SchedulingManager::NORMAL) {
            // Continue job finding procedure somewhere else
            deflectJobRequest(req, source);
        }
    }
}

void SchedulingManager::handleAdoptionOffer(MessageHandle& handle) {

    JobRequest req = Serializable::get<JobRequest>(handle.getRecvData());
    LOG_ADD_SRC(V4_VVER, "Adoption offer for %s", handle.source, 
                    toStr(req.jobId, req.requestedNodeIndex).c_str());

    bool reject = false;
    if (!has(req.jobId)) {
        // Job is not present, so I cannot become a parent!
        reject = true;

    } else {
        // Retrieve concerned job
        Job &job = get(req.jobId);

        // Adoption offer is obsolete if it's internally obsolete or the job's scheduler declines it
        bool obsolete = isAdoptionOfferObsolete(req);
        if (!obsolete) obsolete = _params.reactivationScheduling() 
            && _reactivation_scheduler.hasReactivatorBlockingChild(
                job.getId(), job.getIndex(), req.requestedNodeIndex);

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
    if (_params.reactivationScheduling() && has(req.jobId)) {
        _reactivation_scheduler.processAdoptionOffer(handle.source, req, get(req.jobId), reject);
    }
}

void SchedulingManager::handleRejectionOfDirectedRequest(MessageHandle& handle) {

    OneshotJobRequestRejection rej = Serializable::get<OneshotJobRequestRejection>(handle.getRecvData());
    JobRequest& req = rej.request;
    LOG_ADD_SRC(V5_DEBG, "%s rejected by dormant child", handle.source, 
            toStr(req.jobId, req.requestedNodeIndex).c_str());

    if (!has(req.jobId)) return;

    Job& job = get(req.jobId);
    if (_params.reactivationScheduling() && 
            _reactivation_scheduler.processRejectionOfDirectedRequest(handle.source, rej, job)) {        
        return;
    }

    if (isAdoptionOfferObsolete(req)) return;

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
        deflectJobRequest(req, handle.source);
    }
}

void SchedulingManager::handleAnswerToAdoptionOffer(MessageHandle& handle) {

    IntVec vec = Serializable::get<IntVec>(handle.getRecvData());
    int jobId = vec[0];
    int requestedNodeIndex = vec[1];
    bool accepted = vec[2] == 1;

    // Retrieve according job commitment
    if (!_job_registry.hasCommitment(jobId)) {
        LOG(V4_VVER, "Job commitment for #%i not present despite adoption accept msg\n", jobId);
        return;
    }
    const JobRequest& req = _job_registry.getCommitment(jobId);

    if (req.requestedNodeIndex != requestedNodeIndex || req.requestingNodeRank != handle.source) {
        // Adoption offer answer from invalid rank and/or index
        LOG_ADD_SRC(V4_VVER, "Ignore invalid adoption offer answer concerning #%i:%i\n", 
            handle.source, jobId, requestedNodeIndex);
        return;
    }

    // Retrieve job
    assert(has(jobId));
    Job &job = get(jobId);

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
            uncommit(req.jobId);
            if (job.getState() == SUSPENDED) {
                reactivate(req, handle.source);
            } else {
                execute(req.jobId, handle.source);
            }
        }
        
    } else {
        // Rejected
        LOG_ADD_SRC(V4_VVER, "Rejected to become %s : uncommitting", handle.source, job.toStr());
        uncommit(req.jobId);
        unregisterJobFromBalancer(req.jobId);
        _reactivation_scheduler.suspendReactivator(job);
    }
}

void SchedulingManager::handleQueryForJobDescription(MessageHandle& handle) {

    IntPair pair = Serializable::get<IntPair>(handle.getRecvData());
    int jobId = pair.first;
    int revision = pair.second;

    if (!has(jobId)) return;
    Job& job = get(jobId);

    if (job.getRevision() >= revision) {
        sendJobDescription(jobId, revision, handle.source);
    } else {
        // This revision is not present yet: Defer this query
        // and send the job description upon receiving it
        job.addChildWaitingForRevision(handle.source, revision);
        return;
    }
}

void SchedulingManager::sendJobDescription(int jobId, int revision, int dest) {

    // Retrieve and send concerned job description
    auto& job = get(jobId);
    const auto& descPtr = job.getSerializedDescription(revision);
    assert(descPtr->size() == job.getDescription().getTransferSize(revision) 
        || LOG_RETURN_FALSE("%i != %i\n", descPtr->size(), job.getDescription().getTransferSize(revision)));
    int sendId = MyMpi::isend(dest, MSG_SEND_JOB_DESCRIPTION, descPtr);
    LOG_ADD_DEST(V4_VVER, "Sent job desc. of %s rev. %i, size %lu, id=%i", dest, 
            job.toStr(), revision, descPtr->size(), sendId);
    job.getJobTree().addSendHandle(dest, sendId);
    _send_id_to_job_id[sendId] = jobId;
}

void SchedulingManager::handleJobDescriptionSent(int sendId) {
    auto it = _send_id_to_job_id.find(sendId);
    if (it != _send_id_to_job_id.end()) {
        int jobId = it->second;
        if (has(jobId)) {
            get(jobId).getJobTree().clearSendHandle(sendId);
        }
        _send_id_to_job_id.erase(sendId);
    }
}

void SchedulingManager::handleIncomingJobDescription(MessageHandle& handle) {

    const auto& data = handle.getRecvData();
    int jobId = data.size() >= sizeof(int) ? Serializable::get<int>(data) : -1;
    LOG_ADD_SRC(V4_VVER, "Got desc. of size %i for job #%i", handle.source, data.size(), jobId);
    if (jobId == -1 || !has(jobId)) {
        if (_job_registry.hasCommitment(jobId)) {
            uncommit(jobId);
            unregisterJobFromBalancer(jobId);
            if (has(jobId)) _reactivation_scheduler.suspendReactivator(get(jobId));
        }
        return;
    }

    // Append revision description to job
    auto& job = get(jobId);
    auto dataPtr = std::shared_ptr<std::vector<uint8_t>>(
        new std::vector<uint8_t>(handle.moveRecvData())
    );
    bool valid = appendRevision(jobId, dataPtr, handle.source);
    if (!valid) {
        // Need to clean up shared pointer concurrently 
        // because it might take too much time in the main thread
        ProcessWideThreadPool::get().addTask([sharedPtr = std::move(dataPtr)]() mutable {
            sharedPtr.reset();
        });
        return;
    }

    // If job has not started yet, execute it now
    if (_job_registry.hasCommitment(jobId)) {
        {
            const auto& req = _job_registry.getCommitment(jobId);
            job.setDesiredRevision(req.revision);
            uncommit(jobId);
        }
        execute(jobId, handle.source);
        initiateVolumeUpdate(jobId);
    }
    
    // Job inactive?
    if (job.getState() != ACTIVE) return;

    // Arrived at final revision?
    if (job.getRevision() < job.getDesiredRevision()) {
        // No: Query next revision
        MyMpi::isend(handle.source, MSG_QUERY_JOB_DESCRIPTION, 
            IntPair(jobId, get(jobId).getRevision()+1));
    }
}

void SchedulingManager::handleQueryForExplicitVolumeUpdate(MessageHandle& handle) {

    IntVec payload = Serializable::get<IntVec>(handle.getRecvData());
    int jobId = payload[0];

    // Unknown job? -- ignore.
    if (!has(jobId)) return;

    Job& job = get(jobId);
    int volume = job.getVolume();
    
    // Volume is unknown right now? Query parent recursively. 
    // (Answer will flood back to the entire subtree)
    if (job.getState() == ACTIVE && volume == 0) {
        assert(!job.getJobTree().isRoot());
        MyMpi::isendCopy(job.getJobTree().getParentNodeRank(), MSG_QUERY_VOLUME, handle.getRecvData());
        return;
    }

    // Send response
    IntVec response({jobId, volume, getGlobalBalancingEpoch()});
    LOG_ADD_DEST(V4_VVER, "Answer #%i volume query with v=%i", handle.source, jobId, volume);
    MyMpi::isend(handle.source, MSG_NOTIFY_VOLUME_UPDATE, response);
}

void SchedulingManager::handleExplicitVolumeUpdate(MessageHandle& handle) {

    IntVec recv = Serializable::get<IntVec>(handle.getRecvData());
    int jobId = recv[0];
    int volume = recv[1];
    int balancingEpoch = recv[2];
    if (!has(jobId)) {
        LOG(V1_WARN, "[WARN] Volume update for unknown #%i\n", jobId);
        return;
    }

    // Update volume assignment in job instance (and its children)
    updateVolume(jobId, volume, balancingEpoch, 0);
}

void SchedulingManager::handleLeavingChild(MessageHandle& handle) {

    // Retrieve job
    IntVec recv = Serializable::get<IntVec>(handle.getRecvData());
    int jobId = recv.data[0];
    int index = recv.data[1];
    int rootRank = recv.data[2];

    if (!has(jobId)) {
        MyMpi::isend(rootRank, MSG_NOTIFY_NODE_LEAVING_JOB, handle.moveRecvData());
        return;
    }
    Job& job = get(jobId);

    // Prune away the respective child if necessary
    auto pruned = job.getJobTree().prune(handle.source, index);

    // If necessary, find replacement
    if (pruned != JobTree::TreeRelative::NONE && index < job.getVolume()) {
        LOG(V4_VVER, "%s : look for replacement for %s\n", job.toStr(), toStr(jobId, index).c_str());
        spawnJobRequest(jobId, pruned==JobTree::LEFT_CHILD, getGlobalBalancingEpoch());
    }

    // Initiate communication if the job now became willing to communicate
    job.communicate();
}

void SchedulingManager::handleJobInterruption(MessageHandle& handle) {

    int jobId = Serializable::get<int>(handle.getRecvData());
    if (!has(jobId)) return;

    LOG(V3_VERB, "Acknowledge #%i aborting\n", jobId);
    auto& job = get(jobId);    
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

void SchedulingManager::handleIncrementalJobFinished(MessageHandle& handle) {
    int jobId = Serializable::get<int>(handle.getRecvData());
    if (has(jobId)) {
        LOG(V3_VERB, "Incremental job %s done\n", get(jobId).toStr());
        interruptJob(Serializable::get<int>(handle.getRecvData()), /*terminate=*/true, /*reckless=*/false);
    }
}

void SchedulingManager::handleApplicationMessage(MessageHandle& handle) {
    
    // Deserialize job-specific message
    JobMessage msg = Serializable::get<JobMessage>(handle.getRecvData());

    int jobId = msg.jobId;
    if (!has(jobId)) {
        LOG(V1_WARN, "[WARN] Job message from unknown job #%i\n", jobId);
        if (!msg.returnedToSender) {
            msg.returnedToSender = true;
            MyMpi::isend(handle.source, handle.tag, msg);
        }
        return;
    }

    // Give message to corresponding job
    Job& job = get(jobId);
    job.communicate(handle.source, handle.tag, msg);
}

void SchedulingManager::handleJobResultFound(MessageHandle& handle) {

    // Retrieve job
    IntVec res = Serializable::get<IntVec>(handle.getRecvData());
    int jobId = res[0];
    int revision = res[1];

    // Is the job result invalid or obsolete?
    bool obsolete = false;
    if (!has(jobId) || !get(jobId).getJobTree().isRoot()) {
        obsolete = true;
        LOG(V1_WARN, "[WARN] Invalid adressee for job result of #%i\n", jobId);
    } else if (get(jobId).getRevision() > revision || get(jobId).isRevisionSolved(revision)) {
        obsolete = true;
        LOG_ADD_SRC(V4_VVER, "Discard obsolete result for job #%i rev. %i", handle.source, jobId, revision);
    }
    if (obsolete) {
        MyMpi::isendCopy(handle.source, MSG_NOTIFY_RESULT_OBSOLETE, handle.getRecvData());
        return;
    }
    
    LOG_ADD_SRC(V3_VERB, "#%i rev. %i solved", handle.source, jobId, revision);
    auto& job = get(jobId);
    job.setRevisionSolved(revision);

    // Notify client
    int clientRank = job.getDescription().getClientRank();
    LOG_ADD_DEST(V4_VVER, "%s : inform client job is done", clientRank, job.toStr());
    job.updateVolumeAndUsedCpu(job.getVolume());
    JobStatistics stats;
    stats.jobId = jobId;
    stats.revision = revision;
    stats.successfulRank = handle.source;
    stats.usedWallclockSeconds = job.getAgeSinceActivation();
    stats.usedCpuSeconds = job.getUsedCpuSeconds();
    stats.latencyOf1stVolumeUpdate = job.getLatencyOfFirstVolumeUpdate();

    // Send "Job done!" with statistics to client
    MyMpi::isend(clientRank, MSG_NOTIFY_JOB_DONE, stats);

    // Terminate job and propagate termination message
    if (get(jobId).getDescription().isIncremental()) {
        handleJobInterruption(handle);
    } else {
        interruptJob(Serializable::get<int>(handle.getRecvData()), /*terminate=*/true, /*reckless=*/false);
    }
}

void SchedulingManager::handleQueryForJobResult(MessageHandle& handle) {

    // Receive acknowledgement that the client received the advertised result size
    // and wishes to receive the full job result
    JobStatistics stats; stats.deserialize(handle.getRecvData());
    int jobId = stats.jobId;
    LOG_ADD_DEST(V3_VERB, "Send result of #%i rev. %i to client", handle.source, jobId, stats.revision);
    MyMpi::isend(handle.source, MSG_SEND_JOB_RESULT, _result_store.retrieveSerialization(jobId, stats.revision));
}

void SchedulingManager::handleObsoleteJobResult(MessageHandle& handle) {

    IntVec res = Serializable::get<IntVec>(handle.getRecvData());
    int jobId = res[0];
    int revision = res[1];
    LOG_ADD_SRC(V4_VVER, "job result for #%i rev. %i unwanted", handle.source, jobId, revision);
    _result_store.discard(jobId, revision);
}

void SchedulingManager::handleJobReleasedFromWaitingForReactivation(MessageHandle& handle) {
    IntVec vec = Serializable::get<IntVec>(handle.getRecvData());
    int jobId = vec[0];
    int index = vec[1];
    int epoch = vec[2];
    if (has(jobId) && 
            (get(jobId).getState() != INACTIVE || get(jobId).hasCommitment())) {
        // Job present: release this worker from waiting for that job
        get(jobId).getJobTree().stopWaitingForReactivation(epoch);
        if (_params.hopsUntilCollectiveAssignment() >= 0) _req_matcher->setStatusDirty();
    } else {
        // Job not present any more: Let sender know
        MyMpi::isend(handle.source, MSG_SCHED_NODE_FREED, 
            IntVec({jobId, MyMpi::rank(MPI_COMM_WORLD), index, epoch}));
    }
}




bool SchedulingManager::isRequestObsolete(const JobRequest& req) {

    // Requests for a job root never become obsolete
    if (req.requestedNodeIndex == 0) return false;

    if (req.balancingEpoch < getGlobalBalancingEpoch()) {
        // Request from a past balancing epoch
        LOG(V4_VVER, "%s : past epoch\n", req.toStr().c_str());
        return true;
    }

    if (!has(req.jobId)) return false;

    Job& job = get(req.jobId);
    if (job.getState() == ACTIVE) {
        // Does this node KNOW that the request is already completed?
        if (req.requestedNodeIndex == job.getIndex()
        || (job.getJobTree().hasLeftChild() && req.requestedNodeIndex == job.getJobTree().getLeftChildIndex())
        || (job.getJobTree().hasRightChild() && req.requestedNodeIndex == job.getJobTree().getRightChildIndex())) {
            // Request already completed!
            LOG(V4_VVER, "%s : already completed\n", req.toStr().c_str());
            return true;
        }
    }
    return false;
}

bool SchedulingManager::isAdoptionOfferObsolete(const JobRequest& req, bool alreadyAccepted) {

    // Requests for a job root never become obsolete
    if (req.requestedNodeIndex == 0) return false;

    if (!has(req.jobId)) {
        // Job not known anymore: obsolete
        LOG(V4_VVER, "%s : job unknown\n", req.toStr().c_str());
        return true;
    }

    Job& job = get(req.jobId);
    if (job.getState() != ACTIVE && !_job_registry.hasCommitment(req.jobId)) {
        // Job is not active
        LOG(V4_VVER, "%s : job inactive (%s)\n", req.toStr().c_str(), job.jobStateToStr());
        return true;
    }
    if (req.requestedNodeIndex != job.getJobTree().getLeftChildIndex() 
            && req.requestedNodeIndex != job.getJobTree().getRightChildIndex()) {
        // Requested node index is not a valid child index for this job
        LOG(V4_VVER, "%s : not a valid child index (any more)\n", job.toStr());
        return true;
    }
    if (req.revision < job.getRevision()) {
        // Job was updated in the meantime
        LOG(V4_VVER, "%s : rev. %i not up to date\n", req.toStr().c_str(), req.revision);
        return true;
    }
    if (alreadyAccepted) {
        return false;
    }
    if (req.requestedNodeIndex == job.getJobTree().getLeftChildIndex() && job.getJobTree().hasLeftChild()) {
        // Job already has a left child
        LOG(V4_VVER, "%s : already has left child\n", req.toStr().c_str());
        return true;

    }
    if (req.requestedNodeIndex == job.getJobTree().getRightChildIndex() && job.getJobTree().hasRightChild()) {
        // Job already has a right child
        LOG(V4_VVER, "%s : already has right child\n", req.toStr().c_str());
        return true;
    }
    return false;
}

void SchedulingManager::initiateVolumeUpdate(int jobId) {

    auto& job = get(jobId);
    
    if (_params.explicitVolumeUpdates()) {
        // Volume updates are propagated explicitly
        if (job.getJobTree().isRoot()) {
            // Root worker: update volume (to trigger growth if desired)
            if (job.getVolume() > 1) updateVolume(jobId, job.getVolume(), getGlobalBalancingEpoch(), 0);
        } else {
            // Non-root worker: query parent for the volume of this job
            IntVec payload({jobId});
            MyMpi::isend(job.getJobTree().getParentNodeRank(), MSG_QUERY_VOLUME, payload);
        }
    } else {
        // Volume updates are recognized by each job node independently
        if (getGlobalBalancingEpoch() < job.getBalancingEpochOfLastCommitment()) {
            // Balancing epoch which caused this job node is not present yet
            return;
        }
        // Apply current volume
        if (hasVolume(jobId)) {
            updateVolume(jobId, getVolume(jobId), getGlobalBalancingEpoch(), 0);
        }
    }
}

void SchedulingManager::updateVolume(int jobId, int volume, int balancingEpoch, float eventLatency) {

    // If the job is not in the database, there might be a root request to activate 
    if (!has(jobId)) {
        activateRootRequest(jobId);
        return;
    }

    Job &job = get(jobId);
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

        if (_params.hopsUntilCollectiveAssignment() >= 0) _req_matcher->setStatusDirty();

        // Root of a job updated for the 1st time
        if (tree.isRoot()) {
            if (tree.getBalancingEpochOfLastRequests() == -1) {
                // Job's volume is updated for the first time
                job.setTimeOfFirstVolumeUpdate(Timer::elapsedSeconds());
            }
        }
        
        // Apply volume update to the job's local scheduler
        if (_params.reactivationScheduling()) {
            _reactivation_scheduler.processBalancingUpdate(jobId, job.getIndex(), balancingEpoch, 
                volume, tree.hasLeftChild(), tree.hasRightChild());
        }

        // Handle child relationships with respect to the new volume
        propagateVolumeUpdate(job, volume, balancingEpoch);

        // Update balancing epoch
        tree.setBalancingEpochOfLastRequests(balancingEpoch);
        
        // Shrink (and pause solving) yourself, if necessary
        if (thisIndex > 0 && thisIndex >= volume) {
            LOG(V3_VERB, "%s shrinking\n", job.toStr());
            if (job.getState() == ACTIVE) {
                suspend(jobId);
            } else {
                uncommit(jobId);
                unregisterJobFromBalancer(jobId);
                _reactivation_scheduler.suspendReactivator(job);
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

void SchedulingManager::propagateVolumeUpdate(Job& job, int volume, int balancingEpoch) {

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
            if (_job_registry.hasDormantRoot()) {
                // Becoming an inner node is not acceptable
                // because then the dormant root cannot be restarted seamlessly
                LOG(V4_VVER, "%s cannot grow due to dormant root\n", job.toStr());
                if (job.getState() == ACTIVE) {
                    suspend(jobId);
                } else {
                    uncommit(jobId);
                    unregisterJobFromBalancer(jobId);
                    _reactivation_scheduler.suspendReactivator(job);
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

void SchedulingManager::deflectJobRequest(JobRequest& request, int senderRank) {

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
        _req_matcher->addJobRequest(request);
        return;
    }

    // Get random choice from bounce alternatives
    int nextRank = _routing_tree.getRandomNeighbor();
    if (_routing_tree.getNumNeighbors() > 2) {
        // ... if possible while skipping the requesting node and the sender
        while (nextRank == request.requestingNodeRank || nextRank == senderRank) {
            nextRank = _routing_tree.getRandomNeighbor();
        }
    }

    // Send request to "next" worker node
    LOG_ADD_DEST(V5_DEBG, "Hop %s", nextRank, toStr(request.jobId, request.requestedNodeIndex).c_str());
    MyMpi::isend(nextRank, MSG_REQUEST_NODE, request);
}

void SchedulingManager::activateRootRequest(int jobId) {
    auto optReq = _req_cache.getRootRequest(jobId);
    if (!optReq.has_value()) return;
    LOG(V3_VERB, "Activate %s\n", optReq.value().toStr().c_str());
    deflectJobRequest(optReq.value(), optReq.value().requestingNodeRank);
}

void SchedulingManager::spawnJobRequest(int jobId, bool left, int balancingEpoch) {

    Job& job = get(jobId);
    
    int index = left ? job.getJobTree().getLeftChildIndex() : job.getJobTree().getRightChildIndex();
    if (_params.monoFilename.isSet()) job.getJobTree().updateJobNode(index, index);

    JobRequest req(jobId, job.getApplicationId(), job.getJobTree().getRootNodeRank(), 
            MyMpi::rank(MPI_COMM_WORLD), index, Timer::elapsedSeconds(), balancingEpoch, 0, job.isIncremental());
    req.revision = job.getDesiredRevision();
    int tag = MSG_REQUEST_NODE;    

    emitJobRequest(req, tag, left, -1);
}

void SchedulingManager::emitJobRequest(const JobRequest& req, int tag, bool left, int dest) {

    auto& job = get(req.jobId);

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

void SchedulingManager::commit(JobRequest& req) {
    if (has(req.jobId)) {
        Job& job = get(req.jobId);
        LOG(V3_VERB, "COMMIT %s -> #%i:%i\n", job.toStr(), req.jobId, req.requestedNodeIndex);
        job.commit(req);
        _job_registry.setCommitted();
        
        // Subscribe for volume updates for this job even if the job is not active yet
        // Also reserves a PE of space for this job in case this is a root node
        preregisterJobInBalancer(req.jobId);

        if (_req_matcher) _req_matcher->setStatusDirty();
    }
}

void SchedulingManager::uncommit(int jobId) {
    if (has(jobId)) {
        LOG(V3_VERB, "UNCOMMIT %s\n", get(jobId).toStr());
        get(jobId).uncommit();
        _job_registry.unsetCommitted();
        if (_req_matcher) _req_matcher->setStatusDirty();
    }
}

SchedulingManager::AdoptionResult SchedulingManager::tryAdopt(const JobRequest& req, JobRequestMode mode, int sender, int& removedJob) {

    // Decide whether job should be adopted or bounced to another node
    removedJob = -1;
    
    // Already have another commitment?
    if (_job_registry.committed()) {
        if (_req_matcher) _req_matcher->setStatusDirty();
        return REJECT;
    }

    // Does this node have a dormant root which is NOT this job?
    if (_job_registry.hasDormantRoot() && (
        !has(req.jobId)
        || !get(req.jobId).getJobTree().isRoot() 
        || get(req.jobId).getState() != SUSPENDED
    )) {
        LOG(V4_VVER, "Reject %s : dormant root present\n", req.toStr().c_str());
        return REJECT;
    }

    if (req.requestedNodeIndex > 0 && _reactivation_scheduler.isCommitBlocked(req.jobId, req.requestedNodeIndex)) {
        LOG(V1_WARN, "%s : still have an active scheduler of this node!\n", req.toStr().c_str());
        return REJECT;
    }

    bool isThisDormantRoot = has(req.jobId) && get(req.jobId).getJobTree().isRoot();
    if (isThisDormantRoot) {
        if (req.requestedNodeIndex > 0) {
            // Explicitly avoid to adopt a non-root node of the job of which I have a dormant root
            // (commit would overwrite job index!)
            if (_req_matcher) _req_matcher->setStatusDirty();
            return REJECT;
        }
    } else {
        if (_job_registry.hasDormantRoot() && req.requestedNodeIndex == 0) {
            // Cannot adopt a root node while there is still another dormant root here
            if (_req_matcher) _req_matcher->setStatusDirty();
            return REJECT;
        }
    }

    // Is node idle and not committed to another job?
    if (!_job_registry.isBusyOrCommitted()) {
        if (mode != TARGETED_REJOIN) return ADOPT_FROM_IDLE;
        // Oneshot request: Job must be present and suspended
        else if (_job_registry.hasDormantJob(req.jobId)) {
            return ADOPT_FROM_IDLE;
        } else {
            if (_req_matcher) _req_matcher->setStatusDirty();
            return REJECT;
        }
    }
    // -- node is busy in some form

    // Request for a root node:
    // Possibly adopt the job while dismissing the active job
    if (req.requestedNodeIndex == 0 && !_params.reactivationScheduling()) {

        // Adoption only works if this node does not yet compute for that job
        if (!has(req.jobId) || get(req.jobId).getState() != ACTIVE) {

            // Current job must be a non-root leaf node
            Job& job = _job_registry.getActive();
            if (job.getState() == ACTIVE && !job.getJobTree().isRoot() && job.getJobTree().isLeaf()) {
                
                // Inform parent node of the original job  
                LOG(V4_VVER, "Suspend %s ...\n", job.toStr());
                LOG(V4_VVER, "... to adopt starving %s\n", 
                                toStr(req.jobId, req.requestedNodeIndex).c_str());

                removedJob = job.getId();
                suspend(removedJob);
                return ADOPT_REPLACE_CURRENT;
            }
        }

        // Adoption did not work out: Defer the request if a certain #hops is reached
        if (req.numHops > 0 && req.numHops % std::max(32, MyMpi::size(_comm)) == 0) {
            _req_cache.defer(req, sender);
            return DEFER;
        }
    }

    if (_req_matcher) _req_matcher->setStatusDirty();
    return REJECT;
}

void SchedulingManager::reactivate(const JobRequest& req, int source) {
    // Already has job description: Directly resume job (if not terminated yet)
    assert(has(req.jobId));
    Job& job = get(req.jobId);
    job.updateJobTree(req.requestedNodeIndex, req.rootRank, req.requestingNodeRank);
    setLoad(1, req.jobId);
    assert(!_job_registry.hasCommitment(req.jobId));
    LOG_ADD_SRC(V3_VERB, "RESUME %s", source, 
                toStr(req.jobId, req.requestedNodeIndex).c_str());
    job.resume();

    int demand = job.getDemand();
    _balancer->onActivate(job, demand);
    job.setLastDemand(demand);
}

void SchedulingManager::suspend(int jobId) {
    assert(has(jobId) && get(jobId).getState() == ACTIVE);
    Job& job = get(jobId);
    // Suspend (and possibly erase) job scheduler
    _reactivation_scheduler.suspendReactivator(job);    
    job.suspend();
    setLoad(0, jobId);
    LOG(V3_VERB, "SUSPEND %s\n", job.toStr());
    _balancer->onSuspend(job);
}

void SchedulingManager::terminate(int jobId) {
    assert(has(jobId));
    Job& job = get(jobId);
    bool wasTerminatedBefore = job.getState() == JobState::PAST;
    if (_job_registry.hasActiveJob() && _job_registry.getActive().getId() == jobId) {
        setLoad(0, jobId);
    }

    if (!wasTerminatedBefore) {
        // Gather statistics
        auto numDesires = job.getJobTree().getNumDesires();
        auto numFulfilledDesires = job.getJobTree().getNumFulfiledDesires();
        auto sumDesireLatencies = job.getJobTree().getSumOfDesireLatencies();
        float desireFulfilmentRatio = numDesires == 0 ? 0 : (float)numFulfilledDesires / numDesires;
        float meanFulfilmentLatency = numFulfilledDesires == 0 ? 0 : sumDesireLatencies / numFulfilledDesires;

        auto& latencies = job.getJobTree().getDesireLatencies();
        float meanLatency = 0, minLatency = 0, maxLatency = 0, medianLatency = 0;
        if (!latencies.empty()) {
            std::sort(latencies.begin(), latencies.end());
            meanLatency = std::accumulate(latencies.begin(), latencies.end(), 0.0f) / latencies.size();
            minLatency = latencies.front();
            maxLatency = latencies.back();
            medianLatency = latencies[latencies.size()/2];
        }

        LOG(V3_VERB, "%s desires fulfilled=%.4f latency={num:%i min:%.5f med:%.5f avg:%.5f max:%.5f}\n",
            job.toStr(), desireFulfilmentRatio, latencies.size(), minLatency, medianLatency, meanLatency, maxLatency);
    }

    //_sys_state.addLocal(SYSSTATE_NUMDESIRES, numDesires);
    //_sys_state.addLocal(SYSSTATE_NUMFULFILLEDDESIRES, numFulfilledDesires);
    //_sys_state.addLocal(SYSSTATE_SUMDESIRELATENCIES, sumDesireLatencies);

    job.terminate();
    if (job.hasCommitment()) uncommit(jobId);
    if (!wasTerminatedBefore) _balancer->onTerminate(job);

    LOG(V4_VVER, "Forget %s\n", job.toStr());
    eraseJobAndQueueForDeletion(jobId);
}

void SchedulingManager::interruptJob(int jobId, bool doTerminate, bool reckless) {

    if (!has(jobId)) return;
    Job& job = get(jobId);

    // Ignore if this job node is already in the goal state
    // (also implying that it already forwarded such a request downwards if necessary)
    if (!doTerminate && !job.hasCommitment() && (job.getState() == SUSPENDED || job.getState() == INACTIVE)) 
        return;

    // Propagate message down the job tree
    int msgTag;
    if (doTerminate && reckless) msgTag = MSG_NOTIFY_JOB_ABORTING;
    else if (doTerminate) msgTag = MSG_NOTIFY_JOB_TERMINATING;
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
    if (doTerminate) job.getJobTree().getPastChildren().clear();

    // Uncommit job if committed
    if (job.hasCommitment()) {
        uncommit(jobId);
        unregisterJobFromBalancer(jobId);
        _reactivation_scheduler.suspendReactivator(job);
    }

    // Suspend or terminate the job
    if (doTerminate) terminate(jobId);
    else if (job.getState() == ACTIVE) suspend(jobId);
}

void SchedulingManager::tryAdoptPendingRootActivationRequest() {
    auto optHandle = _req_cache.tryGetPendingRootActivationRequest();
    if (optHandle) {
        handleIncomingJobRequest(optHandle.value(), SchedulingManager::NORMAL);
    }
}

void SchedulingManager::forgetOldJobs() {
    _reactivation_scheduler.forgetInactives();
    auto jobsToForget = _job_registry.findJobsToForget();
    // Perform forgetting of jobs
    for (int jobId : jobsToForget) eraseJobAndQueueForDeletion(jobId);
}

void SchedulingManager::eraseJobAndQueueForDeletion(int jobId) {
    Job* jobPtr;
    {
        Job& job = get(jobId);
        LOG(V4_VVER, "FORGET %s\n", job.toStr());
        if (job.getState() != PAST) job.terminate();
        assert(job.getState() == PAST);
        jobPtr = &job;
    }
    _job_registry.erase(jobPtr);
}

bool SchedulingManager::has(int id) const {return _job_registry.has(id);}
Job& SchedulingManager::get(int id) const {return _job_registry.get(id);}
void SchedulingManager::setLoad(int load, int jobId) {
    _job_registry.setLoad(load, jobId);
    if (_req_matcher) _req_matcher->setStatusDirty();
}

std::string SchedulingManager::toStr(int j, int idx) const {
    return "#" + std::to_string(j) + ":" + std::to_string(idx);
}

void SchedulingManager::triggerMemoryPanic() {
    // Aggressively remove inactive cached jobs
    _job_registry.setMemoryPanic(true);
    forgetOldJobs();
    _job_registry.setMemoryPanic(false);
    // Trigger memory panic in the active job
    if (_job_registry.hasActiveJob()) _job_registry.getActive().appl_memoryPanic();
}

SchedulingManager::~SchedulingManager() {

    // Suspend current job (if applicable) to compute last slice of busy time
    if (_job_registry.hasActiveJob()) 
        setLoad(0, _job_registry.getActive().getId());

    // Setup a watchdog to get feedback on hanging destructors
    Watchdog watchdog(/*enabled=*/_params.watchdog(), /*checkIntervMillis=*/200, Timer::elapsedSeconds());
    watchdog.setWarningPeriod(500);
    watchdog.setAbortPeriod(10*1000);
    
    // Forget each job, move raw pointer to destruct queue
    for (int jobId : _job_registry.collectAllJobs()) {
        eraseJobAndQueueForDeletion(jobId);
        watchdog.reset();
    }

    // Empty destruct queue into garbage for janitor to clean up
    while (_job_registry.hasJobsLeftToDelete()) {
        forgetOldJobs();
        //_janitor_cond_var.notify(); // TODO needed?
        watchdog.reset();
        usleep(10*1000); // 10 milliseconds
    }

    // _job_gc.stop(); TODO required ???
    watchdog.stop();
}
