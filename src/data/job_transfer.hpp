
#pragma once

#include <stdint.h>
#include <vector>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <string>
#include <utility>

#include "serializable.hpp"
#include "data/checksum.hpp"
#include "util/sys/timer.hpp"

// Context ID: unique to a particular worker for a particular job
// on a particular MPI process. Used for unambiguous communication
// among the workers of a job.
typedef unsigned long ctx_id_t;

/**
 * Sent around during the search of a node to adopt the job.
 */
struct JobRequest : public Serializable {

    int jobId {-1};

    int rootRank;
    ctx_id_t rootContextId {0};
    int requestingNodeRank;
    ctx_id_t requestingNodeContextId {0};

    int requestedNodeIndex;
    int revision;
    float timeOfBirth;
    int numHops;
    int balancingEpoch;
    int applicationId;
    bool incremental;

    int multiBaseId {-1};
    int multiplicity {1};
    int multiBegin {-1};
    int multiEnd {-1};

    struct MultiplicityData {
        std::function<void(JobRequest&)> discardCallback;
    } *multiplicityData {nullptr};

public:
    JobRequest() = default;
    JobRequest(int jobId, int applicationId, int rootRank, 
            int requestingNodeRank, int requestedNodeIndex, 
            float timeOfBirth, int balancingEpoch, int numHops, bool incremental) :
        jobId(jobId),
        rootRank(rootRank),
        requestingNodeRank(requestingNodeRank),
        requestedNodeIndex(requestedNodeIndex),
        revision(0),
        timeOfBirth(timeOfBirth),
        numHops(numHops),
        balancingEpoch(balancingEpoch),
        applicationId(applicationId),
        incremental(incremental) {}
    JobRequest& operator=(const JobRequest& other) {
        jobId = other.jobId;
        rootRank = other.rootRank;
        requestingNodeRank = other.requestingNodeRank;
        rootContextId = other.rootContextId;
        requestingNodeContextId = other.requestingNodeContextId;
        requestedNodeIndex = other.requestedNodeIndex;
        revision = other.revision;
        timeOfBirth = other.timeOfBirth;
        numHops = other.numHops;
        balancingEpoch = other.balancingEpoch;
        applicationId = other.applicationId;
        incremental = other.incremental;
        multiBaseId = other.multiBaseId;
        multiplicity = other.multiplicity;
        multiBegin = other.multiBegin;
        multiEnd = other.multiEnd;
        multiplicityData = nullptr;
        return *this;
    }
    JobRequest& operator=(JobRequest&& other) {
        jobId = other.jobId;
        rootRank = other.rootRank;
        requestingNodeRank = other.requestingNodeRank;
        rootContextId = other.rootContextId;
        requestingNodeContextId = other.requestingNodeContextId;
        requestedNodeIndex = other.requestedNodeIndex;
        revision = other.revision;
        timeOfBirth = other.timeOfBirth;
        numHops = other.numHops;
        balancingEpoch = other.balancingEpoch;
        applicationId = other.applicationId;
        incremental = other.incremental;
        multiBaseId = other.multiBaseId;
        multiplicity = other.multiplicity;
        multiBegin = other.multiBegin;
        multiEnd = other.multiEnd;
        multiplicityData = other.multiplicityData;
        other.multiplicityData = nullptr;
        return *this;
    }
    JobRequest(const JobRequest& other) {
        *this = other;
    }
    JobRequest(JobRequest&& other) {
        *this = std::move(other);
    }

    static size_t getMaxTransferSize();
    size_t getTransferSize() const;
    std::vector<uint8_t> serialize() const override;
    JobRequest& deserialize(const std::vector<uint8_t> &packed) override;
    std::string toStr() const;
    bool operator==(const JobRequest& other) const;
    bool operator!=(const JobRequest& other) const;
    bool operator<(const JobRequest& other) const;

    int getMatchingId() const {
        return multiBaseId + (multiplicity==1 ? 0 : 
            (multiplicity - (multiEnd - multiBegin))
        );
    }
    std::pair<JobRequest, JobRequest> getMultipliedChildRequests(int requestingRank = -1, ctx_id_t requestingContextId = 0) {
        std::pair<JobRequest, JobRequest> result;
        if (multiplicity == 1 || multiBegin == -1 || multiEnd == -1) return result;

        int treeRankOffset = multiEnd - multiplicity;
        int myTreeRank = multiBegin - treeRankOffset;
        
        int leftChildTreeRank = 2*myTreeRank+1;
        int leftChildDest = leftChildTreeRank + treeRankOffset;
        if (leftChildDest < multiEnd) {
            result.first = *this;
            result.first.requestedNodeIndex = 2*requestedNodeIndex+1;
            result.first.requestingNodeRank = requestingRank;
            result.first.requestingNodeContextId = requestingContextId;
            result.first.numHops = 0;
            result.first.multiBegin = leftChildDest;
            result.first.timeOfBirth = Timer::elapsedSeconds();
        }

        int rightChildTreeRank = 2*myTreeRank+2;
        int rightChildDest = rightChildTreeRank + treeRankOffset;
        if (rightChildDest < multiEnd) {
            result.second = *this;
            result.second.requestedNodeIndex = 2*requestedNodeIndex+2;
            result.second.requestingNodeRank = requestingRank;
            result.second.requestingNodeContextId = requestingContextId;
            result.second.numHops = 0;
            result.second.multiBegin = rightChildDest;
            result.second.timeOfBirth = Timer::elapsedSeconds();
        }

        return result;
    }
    void setMultiplicityDiscardCallback(std::function<void(JobRequest&)> cb) {
        triggerAndDestroyMultiplicityData();
        multiplicityData = new MultiplicityData{cb};
    }
    void triggerAndDestroyMultiplicityData() {
        if (multiplicityData == nullptr) return;
        multiplicityData->discardCallback(*this);
        delete multiplicityData;
        multiplicityData = nullptr;
    }
    void dismissMultiplicityData() {
        if (multiplicityData == nullptr) return;
        delete multiplicityData;
        multiplicityData = nullptr;
    }
    ~JobRequest() {
        triggerAndDestroyMultiplicityData();
    }
};

struct JobAdoptionOffer : public Serializable {

    JobRequest request;
    ctx_id_t contextId;

    JobAdoptionOffer() = default;
    JobAdoptionOffer(const JobRequest& req, ctx_id_t contextId) : request(req), contextId(contextId) {}

    std::vector<uint8_t> serialize() const override;
    JobAdoptionOffer& deserialize(const std::vector<uint8_t> &packed) override;
};

struct OneshotJobRequestRejection : public Serializable {

    JobRequest request;
    bool isChildStillDormant;

    OneshotJobRequestRejection() = default;
    OneshotJobRequestRejection(const JobRequest& request, bool isChildStillDormant) : 
        request(request), isChildStillDormant(isChildStillDormant) {}
    
    std::vector<uint8_t> serialize() const override;
    OneshotJobRequestRejection& deserialize(const std::vector<uint8_t> &packed) override;
};

struct JobMessage : public Serializable {

    int jobId;
    int treeIndexOfSender {-1};
    int treeIndexOfDestination {-1};
    ctx_id_t contextIdOfSender {0};
    ctx_id_t contextIdOfDestination {0};
    int revision;
    int tag {0};
    int epoch;
    bool returnedToSender = false;
    Checksum checksum;
    std::vector<int> payload;

public:
    JobMessage() {}
    JobMessage(int jobId, ctx_id_t contextIdOfDestination, int revision, int epoch, int tag) : 
        jobId(jobId), contextIdOfDestination(contextIdOfDestination), revision(revision), tag(tag), epoch(epoch) {}
    JobMessage(int jobId, ctx_id_t contextIdOfDestination, int revision, int epoch, int tag, std::initializer_list<int> payload) : 
        jobId(jobId), contextIdOfDestination(contextIdOfDestination), revision(revision), tag(tag), epoch(epoch), payload(payload) {}

    std::vector<uint8_t> serialize() const override;
    JobMessage& deserialize(const std::vector<uint8_t>& packed) override;

    void returnToSender(int senderRank, int mpiTag);
};

struct IntPair : public Serializable {

    int first;
    int second;

public:
    IntPair() = default;
    IntPair(int first, int second) : first(first), second(second) {}

    std::vector<uint8_t> serialize() const override;
    IntPair& deserialize(const std::vector<uint8_t>& packed) override;
};

struct IntVec : public Serializable {

    std::vector<int> data;

public:
    IntVec() = default;
    IntVec(const std::vector<int>& data) : data(data) {}
    IntVec(const std::initializer_list<int>& list) : data(list) {}

    std::vector<uint8_t> serialize() const override;
    IntVec& deserialize(const std::vector<uint8_t>& packed) override;
    int& operator[](const int pos);
};

struct JobStatistics : public Serializable {

    int jobId;
    int revision;
    int successfulRank;
    float usedWallclockSeconds;
    float usedCpuSeconds;
    float latencyOf1stVolumeUpdate;

public:
    JobStatistics() = default;
    
    std::vector<uint8_t> serialize() const override;
    JobStatistics& deserialize(const std::vector<uint8_t>& packed) override;
};
