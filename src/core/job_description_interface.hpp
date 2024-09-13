
#pragma once

#include "comm/msgtags.h"
#include "util/hashing.hpp"
#include "app/job.hpp"
#include "job_registry.hpp"
#include "util/sys/thread_pool.hpp"
#include "comm/msg_queue/message_subscription.hpp"

class JobDescriptionInterface {

private:
    JobRegistry& _job_registry;
    robin_hood::unordered_map<int, int> _send_id_to_job_id;

    std::list<MessageSubscription> _subscriptions;

    const bool _query_desc_skeleton_first;

public:
    JobDescriptionInterface(JobRegistry& jobRegistry, bool queryDescSkeletonFirst) : _job_registry(jobRegistry), _query_desc_skeleton_first(queryDescSkeletonFirst) {

        _subscriptions.emplace_back(MSG_QUERY_JOB_DESCRIPTION,
            [&](auto& h) {handleQueryForJobDescription(h);});
        _subscriptions.emplace_back(MSG_QUERY_JOB_DESCRIPTION_SKELETON,
            [&](auto& h) {handleQueryForJobDescription(h);});

        MyMpi::getMessageQueue().registerSentCallback(MSG_SEND_JOB_DESCRIPTION, [&](int sendId) {
            handleJobDescriptionSent(sendId);
        });
    }

    void updateRevisionAndDescription(Job& job, int revision, int source) {
        job.setDesiredRevision(revision);
        queryNextRevisionIfNeeded(job, source);
    }

    void queryNextRevisionIfNeeded(Job& job, int source) {
        int missingRev;
        if (!job.hasAllDescriptionsForSolving(missingRev)) {
            // Transfer of at least one revision is required
            assert(missingRev >= 0);
            const int msgTag = job.getRevision() < missingRev && _query_desc_skeleton_first ?
                MSG_QUERY_JOB_DESCRIPTION_SKELETON : MSG_QUERY_JOB_DESCRIPTION;
            MyMpi::isend(source, msgTag, IntPair(job.getId(), missingRev));
        }
    }

    bool handleIncomingJobDescription(MessageHandle& handle, int& outJobId) {

        const auto& data = handle.getRecvData();
        outJobId = data.size() >= sizeof(int) ? Serializable::get<int>(data) : -1;
        LOG_ADD_SRC(V4_VVER, "Got desc. of size %lu for job #%i", handle.source, data.size(), outJobId);

        auto dataPtr = std::shared_ptr<std::vector<uint8_t>>(
            new std::vector<uint8_t>(handle.moveRecvData())
        );
        bool valid = _job_registry.has(outJobId) && 
            appendRevision(_job_registry.get(outJobId), dataPtr, handle.source);
        if (!valid) {
            // Need to clean up shared pointer concurrently 
            // because it might take too much time in the main thread
            ProcessWideThreadPool::get().addTask([sharedPtr = std::move(dataPtr)]() mutable {
                sharedPtr.reset();
            });
            return false;
        }
        return true;
    }

    void forwardDescriptionToWaitingChildren(Job& job) {

        // Handle child PEs waiting for the transfer of a revision of this job
        auto& waitingChildren = job.getChildrenWaitingForDescription();
        auto it = waitingChildren.begin();
        while (it != waitingChildren.end()) {
            auto& [rank, rev, sendSkeletonOnly] = *it;
            if (rev > job.getRevision()) {
                ++it;
                continue;
            }
            if (job.getJobTree().hasLeftChild() && job.getJobTree().getLeftChildNodeRank() == rank) {
                // Left child
                send(job, rev, rank, sendSkeletonOnly);
            } else if (job.getJobTree().hasRightChild() && job.getJobTree().getRightChildNodeRank() == rank) {
                // Right child
                send(job, rev, rank, sendSkeletonOnly);
            } // else: obsolete request
            // Remove processed request
            it = waitingChildren.erase(it);
        }
    }

private:

    void send(Job& job, int revision, int dest, bool sendSkeletonOnly) {
        // Retrieve and send concerned job description
        if (sendSkeletonOnly) {
            auto skeleton = job.getSerializedDescriptionSkeleton(revision);
            LOG_ADD_DEST(V4_VVER, "Sending job desc. skeleton of %s rev. %i, size %lu", dest,
                job.toStr(), revision, skeleton.size());
            MyMpi::isend(dest, MSG_SEND_JOB_DESCRIPTION_SKELETON, std::move(skeleton));
        } else {
            assert(!job.getDescription().isRevisionIncomplete(revision));
            const auto& descPtr = job.getSerializedDescription(revision);
            LOG_ADD_DEST(V4_VVER, "Sending job desc. of %s rev. %i, size %lu, formula begins at idx %lu", dest,
                    job.toStr(), revision, descPtr->size(),
                    ((const uint8_t*)job.getDescription().getFormulaPayload(revision) - job.getDescription().getRevisionData(revision)->data()));
            assert(descPtr->size() == job.getDescription().getTransferSize(revision) 
                || LOG_RETURN_FALSE("%i != %i\n", descPtr->size(), job.getDescription().getTransferSize(revision)));
            int sendId = MyMpi::isend(dest, MSG_SEND_JOB_DESCRIPTION, descPtr);
            job.getJobTree().addSendHandle(dest, sendId);
            _send_id_to_job_id[sendId] = job.getId();
            LOG_ADD_DEST(V4_VVER, "Sent id=%i", dest, sendId);
        }
    }

    bool appendRevision(Job& job, const std::shared_ptr<std::vector<uint8_t>>& description, int source) {

        int jobId = job.getId();
        int rev = JobDescription::readRevisionIndex(*description);
        if (job.hasDescription()) {
            if (rev > job.getMaxConsecutiveRevision()+1) {
                // Revision data would cause a "hole" in the list of job revision data
                LOG(V1_WARN, "[WARN] #%i rev. %i inconsistent w/ max. consecutive rev. %i : discard desc. of size %lu\n", 
                    jobId, rev, job.getMaxConsecutiveRevision(), description->size());
                return false;
            }
            if (job.getRevision() >= rev && !job.getDescription().isRevisionIncomplete(rev)) {
                LOG(V1_WARN, "[WARN] #%i rev. %i already fully present : discard desc. of size %lu\n", 
                    jobId, rev, description->size());
                return false;
            }
        } else if (rev != 0) {
            LOG(V1_WARN, "[WARN] #%i invalid \"first\" rev. %i : discard desc. of size %lu\n", jobId, rev, description->size());
                return false;
        }

        // Push revision description
        job.pushRevision(description);
        return true;
    }

    void handleJobDescriptionSent(int sendId) {
        auto it = _send_id_to_job_id.find(sendId);
        if (it != _send_id_to_job_id.end()) {
            int jobId = it->second;
            if (_job_registry.has(jobId)) {
                _job_registry.get(jobId).getJobTree().clearSendHandle(sendId);
            }
            _send_id_to_job_id.erase(sendId);
        }
    }

    void handleQueryForJobDescription(MessageHandle& handle) {

        IntPair pair = Serializable::get<IntPair>(handle.getRecvData());
        int jobId = pair.first;
        int revision = pair.second;
        int tag = handle.tag;
        bool sendSkeletonOnly = tag == MSG_QUERY_JOB_DESCRIPTION_SKELETON;

        if (!_job_registry.has(jobId)) return;
        Job& job = _job_registry.get(jobId);

        if (job.getRevision() >= revision && (sendSkeletonOnly || !job.getDescription().isRevisionIncomplete(revision))) {
            send(job, revision, handle.source, sendSkeletonOnly);
        } else {
            // This revision is not present yet: Defer this query
            // and send the job description upon receiving it
            job.addChildWaitingForRevision(handle.source, revision, sendSkeletonOnly);
            return;
        }
    }
};
