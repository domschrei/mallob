
#pragma once

#include <vector>
#include <cstring>
#include <sstream>

#include "serializable.hpp"
#include "data/checksum.hpp"
#include "data/job_description.hpp"
#include "util/sys/timer.hpp"

/**
 * Sent around during the search of a node to adopt the job.
 */
struct JobRequest : public Serializable {

    int jobId {-1};
    int rootRank;
    int requestingNodeRank;
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
    std::pair<JobRequest, JobRequest> getMultipliedChildRequests(int requestingRank) {
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

struct OneshotJobRequestRejection : public Serializable {

    JobRequest request;
    bool isChildStillDormant;

    OneshotJobRequestRejection() = default;
    OneshotJobRequestRejection(const JobRequest& request, bool isChildStillDormant) : 
        request(request), isChildStillDormant(isChildStillDormant) {}
    
    std::vector<uint8_t> serialize() const override;
    OneshotJobRequestRejection& deserialize(const std::vector<uint8_t> &packed) override;
};

struct WorkRequest : public Serializable {
    int requestingRank;
    int numHops;
    int balancingEpoch;

    WorkRequest() = default;
    WorkRequest(int rank, int balancingEpoch) : requestingRank(rank), numHops(0), balancingEpoch(balancingEpoch) {}

    std::vector<uint8_t> serialize() const override;
    WorkRequest& deserialize(const std::vector<uint8_t> &packed) override;
};

struct WorkRequestComparator {
    bool operator()(const WorkRequest& lhs, const WorkRequest& rhs) const;
};

/**
 * Sent as pre-information on a job that will be transferred
 * based on a previous commitment.
 */
struct JobSignature : public Serializable {

    int jobId;
    int rootRank;
    int firstIncludedRevision;
    size_t transferSize;

public:
    JobSignature() = default;

    JobSignature(int jobId, int rootRank, int firstIncludedRevision, size_t transferSize) :
        jobId(jobId),
        rootRank(rootRank),
        firstIncludedRevision(firstIncludedRevision),
        transferSize(transferSize) {}

    int getTransferSize() const;
    std::vector<uint8_t> serialize() const override;
    JobSignature& deserialize(const std::vector<uint8_t>& packed) override;
};

struct JobMessage : public Serializable {

    int jobId;
    int revision;
    int tag;
    int epoch;
    bool returnedToSender = false;
    Checksum checksum;
    std::vector<int> payload;

public:
    JobMessage() {}
    JobMessage(int jobId, int revision, int epoch, int tag) : 
        jobId(jobId), revision(revision), tag(tag), epoch(epoch) {}
    JobMessage(int jobId, int revision, int epoch, int tag, std::initializer_list<int> payload) : 
        jobId(jobId), revision(revision), tag(tag), epoch(epoch), payload(payload) {}

    std::vector<uint8_t> serialize() const override;
    JobMessage& deserialize(const std::vector<uint8_t>& packed) override;
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
