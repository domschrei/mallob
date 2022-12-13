
#pragma once

#include "app/job.hpp"
#include "util/tsl/robin_map.h"
#include "scheduling/local_scheduler.hpp"
#include "comm/msg_queue/message_subscription.hpp"
#include "job_registry.hpp"

class ReactivationScheduler {

private:
    Parameters& _params;
    JobRegistry& _job_registry;

    robin_hood::unordered_map<std::pair<int, int>, LocalScheduler, IntPairHasher> _schedulers;

    EmitDirectedJobRequestCallback _cb_emit_job_request;

    std::list<MessageSubscription> _subscriptions;

public:
    ReactivationScheduler(Parameters& params, JobRegistry& jobRegistry, EmitDirectedJobRequestCallback emitJobReq) : 
            _params(params), _job_registry(jobRegistry), _cb_emit_job_request(emitJobReq) {

        _subscriptions.emplace_back(MSG_SCHED_NODE_FREED, 
            [&](auto& h) {handleNodeFreedFromReactivation(h);});
    }

    void initializeReactivator(const JobRequest& req, Job& job) {

        auto key = std::pair<int, int>(req.jobId, req.requestedNodeIndex);
        if (!_schedulers.count(key)) {
            _schedulers.emplace(key, constructScheduler(job));
            _job_registry.incrementNumReactivators(req.jobId);
        }

        if (job.getJobTree().isRoot()) {
            if (req.revision == 0) {
                // Initialize the root's local scheduler, which in turn initializes
                // the scheduler of each child node (recursively)
                JobSchedulingUpdate update;
                update.epoch = req.balancingEpoch;
                if (update.epoch < 0) update.epoch = 0;
                _schedulers.at(key).initializeScheduling(update, job.getJobTree().getParentNodeRank());
            } else {
                _schedulers.at(key).beginResumptionAsRoot();
            }
        } else {
            assert(_schedulers.at(key).canCommit());
            _schedulers.at(key).resetRole();
        }
    }

    void suspendReactivator(Job& job) {
        auto key = std::pair<int, int>(job.getId(), job.getIndex());
        if (_schedulers.count(key)) {
            auto& sched = _schedulers.at(key);
            if (sched.isActive()) sched.beginSuspension();
            if (job.getIndex() > 0 && sched.canCommit()) {
                _schedulers.erase(key);
                _job_registry.decrementNumReactivators(job.getId());
            }
        }
    }

    void handle(MessageHandle& handle) {
        auto [jobId, index] = Serializable::get<IntPair>(handle.getRecvData());
        if (hasScheduler(jobId, index)) 
            getScheduler(jobId, index).handle(handle);
        else if (handle.tag == MSG_SCHED_INITIALIZE_CHILD_WITH_NODES) {
            // A scheduling package for an unknown job arrived:
            // it is important to return this package to the sender
            JobSchedulingUpdate update; update.deserialize(handle.getRecvData());
            update.destinationIndex = (update.destinationIndex-1) / 2;
            MyMpi::isend(handle.source, MSG_SCHED_RETURN_NODES, update);
        }
    }

    void handleNodeFreedFromReactivation(MessageHandle& handle) {
        IntVec vec = Serializable::get<IntVec>(handle.getRecvData());
        int jobId = vec[0];
        int rank = vec[1];
        int index = vec[2];
        int epoch = vec[3];
        // If an active job scheduler is present, erase the freed node from local inactive nodes
        // (may not always succeed if the job has shrunk in the meantime, but is not essential for correctness)
        if (hasScheduler(jobId, index)) {
            getScheduler(jobId, index).handleNodeFreed(rank, epoch, index);
        }
    }

    void processAdoptionOffer(int source, JobRequest& req, Job& job, bool reject) {

        if (!hasScheduler(req.jobId, job.getIndex())) return;
        auto& scheduler = getScheduler(req.jobId, job.getIndex());

        if (!reject) {
            // Adoption accepted
            scheduler.handleChildJoining(source, req.balancingEpoch, req.requestedNodeIndex);
        } else {
            // Adoption declined
            bool hasChild = req.requestedNodeIndex == job.getJobTree().getLeftChildIndex() ?
                job.getJobTree().hasLeftChild() : job.getJobTree().hasRightChild();
            scheduler.handleRejectReactivation(source, req.balancingEpoch, 
                req.requestedNodeIndex, /*lost=*/false, hasChild);
        }
    }

    bool processRejectionOfDirectedRequest(int source, OneshotJobRequestRejection& rej, Job& job) {

        JobRequest& req = rej.request;
        if (!hasScheduler(req.jobId, job.getIndex())) return false;
        
        bool hasChild = req.requestedNodeIndex == job.getJobTree().getLeftChildIndex() ?
            job.getJobTree().hasLeftChild() : job.getJobTree().hasRightChild();
        getScheduler(req.jobId, job.getIndex()).handleRejectReactivation(source, req.balancingEpoch, 
            req.requestedNodeIndex, !rej.isChildStillDormant, hasChild);
        return true;
    }

    void processBalancingUpdate(int jobId, int index, int balancingEpoch, int volume, 
                bool hasLeft, bool hasRight) {

        if (!hasScheduler(jobId, index)) return;
        getScheduler(jobId, index).updateBalancing(balancingEpoch, volume, hasLeft, hasRight);
    }

    bool hasReactivatorBlockingChild(int jobId, int index, int requestedNodeIndex) {
        return hasScheduler(jobId, index) && !getScheduler(jobId, index).acceptsChild(requestedNodeIndex);
    }

    bool isCommitBlocked(int jobId, int index) {
        return hasScheduler(jobId, index) && !getScheduler(jobId, index).canCommit();
    }

    void forgetInactives() {
        auto it = _schedulers.begin();
        while (it != _schedulers.end()) {
            auto& [id, idx] = it->first;
            bool hasJob = _job_registry.has(id);
            Job* job = hasJob ? &_job_registry.get(id) : nullptr;
            if (hasJob && (job->getState() == ACTIVE || job->hasCommitment())) {
                ++it;
                continue;
            }
            LocalScheduler& scheduler = it->second;
            // Condition for a scheduler to be deleted:
            // Either job is not present (any more) or non-root
            if ((!hasJob || idx > 0) && scheduler.canCommit()) {
                it = _schedulers.erase(it);
                _job_registry.decrementNumReactivators(id);
            } else ++it;
        }
    }

private:

    LocalScheduler constructScheduler(Job& job) {
        LocalScheduler scheduler(job.getId(), _params, job.getJobTree(), 
        [&job, this](int epoch, bool left, int dest) {
            JobRequest req = job.spawnJobRequest(left, epoch);
            _cb_emit_job_request(req, dest==-1 ? MSG_REQUEST_NODE : MSG_REQUEST_NODE_ONESHOT, left, dest);
        });
        return scheduler;
    }

    bool hasScheduler(int jobId, int index) const {return _schedulers.count(std::pair<int, int>(jobId, index));}
    LocalScheduler& getScheduler(int jobId, int index) {return _schedulers.at(std::pair<int, int>(jobId, index));}
};
