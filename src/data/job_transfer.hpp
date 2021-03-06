
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
    JobDescription::Application application;
    int rootRank;
    int requestingNodeRank;
    int requestedNodeIndex;
    int currentRevision;
    int lastKnownRevision;
    float timeOfBirth;
    int numHops;
    int balancingEpoch;

public:
    JobRequest() = default;

    JobRequest(int jobId, JobDescription::Application application, int rootRank, 
            int requestingNodeRank, int requestedNodeIndex, 
            float timeOfBirth, int balancingEpoch, int numHops) :
        jobId(jobId),
        application(application),
        rootRank(rootRank),
        requestingNodeRank(requestingNodeRank),
        requestedNodeIndex(requestedNodeIndex),
        currentRevision(0),
        lastKnownRevision(-1),
        timeOfBirth(timeOfBirth),
        numHops(numHops), 
        balancingEpoch(balancingEpoch) {}

    std::vector<uint8_t> serialize() const override {
        int size = (8*sizeof(int)+sizeof(float)+sizeof(JobDescription::Application));
        std::vector<uint8_t> packed(size);
        int i = 0, n;
        n = sizeof(int); memcpy(packed.data()+i, &jobId, n); i += n;
        n = sizeof(JobDescription::Application); memcpy(packed.data()+i, &application, n); i += n;
        n = sizeof(int); memcpy(packed.data()+i, &rootRank, n); i += n;
        n = sizeof(int); memcpy(packed.data()+i, &requestingNodeRank, n); i += n;
        n = sizeof(int); memcpy(packed.data()+i, &requestedNodeIndex, n); i += n;
        n = sizeof(int); memcpy(packed.data()+i, &currentRevision, n); i += n;
        n = sizeof(int); memcpy(packed.data()+i, &lastKnownRevision, n); i += n;
        n = sizeof(float); memcpy(packed.data()+i, &timeOfBirth, n); i += n;
        n = sizeof(int); memcpy(packed.data()+i, &numHops, n); i += n;
        n = sizeof(int); memcpy(packed.data()+i, &balancingEpoch, n); i += n;
        return packed;
    }

    JobRequest& deserialize(const std::vector<uint8_t> &packed) override {
        int i = 0, n;
        n = sizeof(int); memcpy(&jobId, packed.data()+i, n); i += n;
        n = sizeof(JobDescription::Application); memcpy(&application, packed.data()+i, n); i += n;
        n = sizeof(int); memcpy(&rootRank, packed.data()+i, n); i += n;
        n = sizeof(int); memcpy(&requestingNodeRank, packed.data()+i, n); i += n;
        n = sizeof(int); memcpy(&requestedNodeIndex, packed.data()+i, n); i += n;
        n = sizeof(int); memcpy(&currentRevision, packed.data()+i, n); i += n;
        n = sizeof(int); memcpy(&lastKnownRevision, packed.data()+i, n); i += n;
        n = sizeof(float); memcpy(&timeOfBirth, packed.data()+i, n); i += n;
        n = sizeof(int); memcpy(&numHops, packed.data()+i, n); i += n;
        n = sizeof(int); memcpy(&balancingEpoch, packed.data()+i, n); i += n;
        return *this;
    }

    std::string toStr() const {
        std::ostringstream out;
        out.precision(3);
        out << std::fixed << timeOfBirth;
        auto birthStr = out.str();
        return "r.#" + std::to_string(jobId) + ":" + std::to_string(requestedNodeIndex) 
                + " rev. " + std::to_string(currentRevision) + " <- [" 
                + std::to_string(requestingNodeRank) + "] born=" + birthStr 
                + " hops=" + std::to_string(numHops)
                + " epoch=" + std::to_string(balancingEpoch);
    }

    bool operator==(const JobRequest& other) const {
        return jobId == other.jobId 
            && requestedNodeIndex == other.requestedNodeIndex 
            && balancingEpoch == other.balancingEpoch
            && currentRevision == other.currentRevision
            && numHops == other.numHops;
    }
    bool operator!=(const JobRequest& other) const {
        return !(*this == other);
    }
};

struct OneshotJobRequestRejection : public Serializable {

    JobRequest request;
    bool isChildStillDormant;

    OneshotJobRequestRejection() = default;
    OneshotJobRequestRejection(const JobRequest& request, bool isChildStillDormant) : 
        request(request), isChildStillDormant(isChildStillDormant) {}
    
    std::vector<uint8_t> serialize() const override {
        std::vector<uint8_t> packed = request.serialize();
        size_t sizeBefore = packed.size();
        packed.resize(packed.size()+sizeof(bool));
        memcpy(packed.data()+sizeBefore, &isChildStillDormant, sizeof(bool));
        return packed;
    }

    OneshotJobRequestRejection& deserialize(const std::vector<uint8_t> &packed) override {
        request.deserialize(packed);
        memcpy(&isChildStillDormant, packed.data()+packed.size()-sizeof(bool), sizeof(bool));
        return *this;
    }
};

struct WorkRequest : public Serializable {
    int requestingRank;
    int numHops;
    int balancingEpoch;

    WorkRequest() = default;
    WorkRequest(int rank, int balancingEpoch) : requestingRank(rank), numHops(0), balancingEpoch(balancingEpoch) {}

    std::vector<uint8_t> serialize() const override {
        std::vector<uint8_t> packed(3*sizeof(int));
        int i = 0, n;
        n = sizeof(int); memcpy(packed.data()+i, &requestingRank, n); i += n;
        n = sizeof(int); memcpy(packed.data()+i, &numHops, n); i += n;
        n = sizeof(int); memcpy(packed.data()+i, &balancingEpoch, n); i += n;
        return packed;
    }

    WorkRequest& deserialize(const std::vector<uint8_t> &packed) override {
        int i = 0, n;
        n = sizeof(int); memcpy(&requestingRank, packed.data()+i, n); i += n;
        n = sizeof(int); memcpy(&numHops, packed.data()+i, n); i += n;
        n = sizeof(int); memcpy(&balancingEpoch, packed.data()+i, n); i += n;
        return *this;
    }
};

struct WorkRequestComparator {
    bool operator()(const WorkRequest& lhs, const WorkRequest& rhs) const {
        if (lhs.balancingEpoch != rhs.balancingEpoch) return lhs.balancingEpoch > rhs.balancingEpoch;
        if (lhs.numHops != rhs.numHops) return lhs.numHops < rhs.numHops;
        return std::hash<int>()(lhs.requestingRank) < std::hash<int>()(rhs.requestingRank);
    }
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

    int getTransferSize() const {
        return transferSize;
    }

    std::vector<uint8_t> serialize() const override {
        int size = (3*sizeof(int) + sizeof(size_t));
        std::vector<uint8_t> packed(size);

        int i = 0, n;
        n = sizeof(int);    memcpy(packed.data()+i, &jobId, n); i += n;
        n = sizeof(int);    memcpy(packed.data()+i, &rootRank, n); i += n;
        n = sizeof(int);    memcpy(packed.data()+i, &firstIncludedRevision, n); i += n;
        n = sizeof(size_t); memcpy(packed.data()+i, &transferSize, n); i += n;
        return packed;
    }

    JobSignature& deserialize(const std::vector<uint8_t>& packed) override {
        int i = 0, n;
        n = sizeof(int);    memcpy(&jobId, packed.data()+i, n); i += n;
        n = sizeof(int);    memcpy(&rootRank, packed.data()+i, n); i += n;
        n = sizeof(int);    memcpy(&firstIncludedRevision, packed.data()+i, n); i += n;
        n = sizeof(size_t); memcpy(&transferSize, packed.data()+i, n); i += n;
        return *this;
    }
};

struct JobMessage : public Serializable {

    int jobId;
    int revision;
    int tag;
    int epoch;
    Checksum checksum;
    std::vector<int> payload;

public:
    std::vector<uint8_t> serialize() const override {
        int size = 4*sizeof(int) + payload.size()*sizeof(int) + sizeof(Checksum);
        std::vector<uint8_t> packed(size);

        int i = 0, n;
        n = sizeof(int); memcpy(packed.data()+i, &jobId, n); i += n;
        n = sizeof(int); memcpy(packed.data()+i, &revision, n); i += n;
        n = sizeof(int); memcpy(packed.data()+i, &tag, n); i += n;
        n = sizeof(int); memcpy(packed.data()+i, &epoch, n); i += n;
        n = sizeof(Checksum); memcpy(packed.data()+i, &checksum, n); i += n;
        n = payload.size()*sizeof(int); memcpy(packed.data()+i, payload.data(), n); i += n;
        return packed;
    }

    JobMessage& deserialize(const std::vector<uint8_t>& packed) override {
        int i = 0, n;
        n = sizeof(int); memcpy(&jobId, packed.data()+i, n); i += n;
        n = sizeof(int); memcpy(&revision, packed.data()+i, n); i += n;
        n = sizeof(int); memcpy(&tag, packed.data()+i, n); i += n;
        n = sizeof(int); memcpy(&epoch, packed.data()+i, n); i += n;
        n = sizeof(Checksum); memcpy(&checksum, packed.data()+i, n); i += n;
        n = packed.size()-i; payload.resize(n/sizeof(int)); 
        memcpy(payload.data(), packed.data()+i, n); i += n;
        return *this;
    }
};

struct IntPair : public Serializable {

    int first;
    int second;

public:
    IntPair() = default;
    IntPair(int first, int second) : first(first), second(second) {}

    std::vector<uint8_t> serialize() const override {
        int size = (2*sizeof(int));
        std::vector<uint8_t> packed(size);
        int i = 0, n;
        n = sizeof(int); memcpy(packed.data()+i, &first, n); i += n;
        n = sizeof(int); memcpy(packed.data()+i, &second, n); i += n;
        return packed;
    }

    IntPair& deserialize(const std::vector<uint8_t>& packed) override {
        int i = 0, n;
        n = sizeof(int); memcpy(&first, packed.data()+i, n); i += n;
        n = sizeof(int); memcpy(&second, packed.data()+i, n); i += n;
        return *this;
    }
};

struct IntVec : public Serializable {

    std::vector<int> data;

public:
    IntVec() = default;
    IntVec(const std::vector<int>& data) : data(data) {}
    IntVec(const std::initializer_list<int>& list) : data(list) {}

    std::vector<uint8_t> serialize() const override {
        int size = (data.size()*sizeof(int));
        std::vector<uint8_t> packed(size);
        memcpy(packed.data(), data.data(), size);
        return packed;
    }

    IntVec& deserialize(const std::vector<uint8_t>& packed) override {
        data.resize(packed.size() / sizeof(int));
        memcpy(data.data(), packed.data(), packed.size());
        return *this;
    }

    int& operator[](const int pos) {
        return data[pos];   
    }
};

#endif
