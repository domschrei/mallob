
#ifndef DOMPASCH_JOB_TRANSFER
#define DOMPASCH_JOB_TRANSFER

#include <vector>
#include <cstring>
#include <sstream>

#include "serializable.hpp"
#include "data/checksum.hpp"
#include "data/job_description.hpp"

/**
 * Sent around during the search of a node to adopt the job.
 */
struct JobRequest : public Serializable {

    int jobId;
    int rootRank;
    int requestingNodeRank;
    int requestedNodeIndex;
    int revision;
    float timeOfBirth;
    int numHops;
    int balancingEpoch;
    JobDescription::Application application;

public:
    JobRequest() = default;

    JobRequest(int jobId, JobDescription::Application application, int rootRank, 
            int requestingNodeRank, int requestedNodeIndex, 
            float timeOfBirth, int balancingEpoch, int numHops) :
        jobId(jobId),
        rootRank(rootRank),
        requestingNodeRank(requestingNodeRank),
        requestedNodeIndex(requestedNodeIndex),
        revision(0),
        timeOfBirth(timeOfBirth),
        numHops(numHops),
        balancingEpoch(balancingEpoch),
        application(application) {}

    static size_t getTransferSize();
    std::vector<uint8_t> serialize() const override;
    JobRequest& deserialize(const std::vector<uint8_t> &packed) override;
    std::string toStr() const;
    bool operator==(const JobRequest& other) const;
    bool operator!=(const JobRequest& other) const;
    bool operator<(const JobRequest& other) const;
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

#endif
