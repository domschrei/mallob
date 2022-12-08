
#pragma once

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

public:
    JobDescriptionInterface(JobRegistry& jobRegistry) : _job_registry(jobRegistry) {

        _subscriptions.emplace_back(MSG_QUERY_JOB_DESCRIPTION,
            [&](auto& h) {handleQueryForJobDescription(h);});

        MyMpi::getMessageQueue().registerSentCallback(MSG_SEND_JOB_DESCRIPTION, [&](int sendId) {
            handleJobDescriptionSent(sendId);
        });
    }

    void updateRevisionAndDescription(Job& job, int revision, int source) {

        job.setDesiredRevision(revision);
        if (!job.hasDescription() || job.getRevision() < revision) {
            // Transfer of at least one revision is required
            int requestedRevision = job.hasDescription() ? job.getRevision()+1 : 0;
            MyMpi::isend(source, MSG_QUERY_JOB_DESCRIPTION, IntPair(job.getId(), requestedRevision));
        }
    }

    void queryNextRevisionIfNeeded(Job& job, int source) {

        // Arrived at final revision?
        if (job.getRevision() < job.getDesiredRevision()) {
            // No: Query next revision
            MyMpi::isend(source, MSG_QUERY_JOB_DESCRIPTION, 
                IntPair(job.getId(), job.getRevision()+1));
        }
    }

    bool handleIncomingJobDescription(MessageHandle& handle, int& outJobId) {

        const auto& data = handle.getRecvData();
        outJobId = data.size() >= sizeof(int) ? Serializable::get<int>(data) : -1;
        LOG_ADD_SRC(V4_VVER, "Got desc. of size %i for job #%i", handle.source, data.size(), outJobId);

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
                send(job, rev, rank);
            } else if (job.getJobTree().hasRightChild() && job.getJobTree().getRightChildNodeRank() == rank) {
                // Right child
                send(job, rev, rank);
            } // else: obsolete request
            // Remove processed request
            it = waitingRankRevPairs.erase(it);
        }
    }

private:

    void send(Job& job, int revision, int dest) {
        // Retrieve and send concerned job description
        const auto& descPtr = job.getSerializedDescription(revision);
        assert(descPtr->size() == job.getDescription().getTransferSize(revision) 
            || LOG_RETURN_FALSE("%i != %i\n", descPtr->size(), job.getDescription().getTransferSize(revision)));
        int sendId = MyMpi::isend(dest, MSG_SEND_JOB_DESCRIPTION, descPtr);
        LOG_ADD_DEST(V4_VVER, "Sent job desc. of %s rev. %i, size %lu, id=%i", dest, 
                job.toStr(), revision, descPtr->size(), sendId);
        job.getJobTree().addSendHandle(dest, sendId);
        _send_id_to_job_id[sendId] = job.getId();
    }

    bool appendRevision(Job& job, const std::shared_ptr<std::vector<uint8_t>>& description, int source) {

        int jobId = job.getId();
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

        if (!_job_registry.has(jobId)) return;
        Job& job = _job_registry.get(jobId);

        if (job.getRevision() >= revision) {
            send(job, revision, handle.source);
        } else {
            // This revision is not present yet: Defer this query
            // and send the job description upon receiving it
            job.addChildWaitingForRevision(handle.source, revision);
            return;
        }
    }

};
