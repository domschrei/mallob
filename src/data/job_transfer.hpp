
#ifndef DOMPASCH_JOB_TRANSFER
#define DOMPASCH_JOB_TRANSFER

#include <vector>
#include <cstring>

#include "serializable.hpp"

/**
 * Sent around during the search of a node to adopt the job.
 */
struct JobRequest : public Serializable {

    int jobId;
    int rootRank;
    int requestingNodeRank;
    int requestedNodeIndex;
    int fullTransfer;
    int revision;
    float timeOfBirth;
    int numHops;

public:
    JobRequest() = default;

    JobRequest(int jobId, int rootRank, int requestingNodeRank, int requestedNodeIndex, float timeOfBirth, int numHops = 0) :
        jobId(jobId),
        rootRank(rootRank),
        requestingNodeRank(requestingNodeRank),
        requestedNodeIndex(requestedNodeIndex),
        fullTransfer(1),
        revision(0),
        timeOfBirth(timeOfBirth),
        numHops(numHops) {}

    std::shared_ptr<std::vector<uint8_t>> serialize() const override {
        int size = (7*sizeof(int)+sizeof(float));
        std::shared_ptr<std::vector<uint8_t>> packed = std::make_shared<std::vector<uint8_t>>(size);
        int i = 0, n;
        n = sizeof(int); memcpy(packed->data()+i, &jobId, n); i += n;
        n = sizeof(int); memcpy(packed->data()+i, &rootRank, n); i += n;
        n = sizeof(int); memcpy(packed->data()+i, &requestingNodeRank, n); i += n;
        n = sizeof(int); memcpy(packed->data()+i, &requestedNodeIndex, n); i += n;
        n = sizeof(int); memcpy(packed->data()+i, &fullTransfer, n); i += n;
        n = sizeof(int); memcpy(packed->data()+i, &revision, n); i += n;
        n = sizeof(float); memcpy(packed->data()+i, &timeOfBirth, n); i += n;
        n = sizeof(int); memcpy(packed->data()+i, &numHops, n); i += n;
        return packed;
    }

    JobRequest& deserialize(const std::vector<uint8_t> &packed) override {
        int i = 0, n;
        n = sizeof(int); memcpy(&jobId, packed.data()+i, n); i += n;
        n = sizeof(int); memcpy(&rootRank, packed.data()+i, n); i += n;
        n = sizeof(int); memcpy(&requestingNodeRank, packed.data()+i, n); i += n;
        n = sizeof(int); memcpy(&requestedNodeIndex, packed.data()+i, n); i += n;
        n = sizeof(int); memcpy(&fullTransfer, packed.data()+i, n); i += n;
        n = sizeof(int); memcpy(&revision, packed.data()+i, n); i += n;
        n = sizeof(float); memcpy(&timeOfBirth, packed.data()+i, n); i += n;
        n = sizeof(int); memcpy(&numHops, packed.data()+i, n); i += n;
        return *this;
    }
};

/**
 * Sent as pre-information on a job that will be transferred
 * based on a previous commitment.
 */
struct JobSignature : public Serializable {

    int jobId;
    int rootRank;
    int revision;
    size_t transferSize;

public:
    JobSignature() = default;

    JobSignature(int jobId, int rootRank, int revision, size_t transferSize) :
        jobId(jobId),
        rootRank(rootRank),
        revision(revision),
        transferSize(transferSize) {}

    int getTransferSize() const {
        return transferSize;
    }

    std::shared_ptr<std::vector<uint8_t>> serialize() const override {
        int size = (3*sizeof(int) + sizeof(size_t));
        std::shared_ptr<std::vector<uint8_t>> packed = std::make_shared<std::vector<uint8_t>>(size);

        int i = 0, n;
        n = sizeof(int);    memcpy(packed->data()+i, &jobId, n); i += n;
        n = sizeof(int);    memcpy(packed->data()+i, &rootRank, n); i += n;
        n = sizeof(int);    memcpy(packed->data()+i, &revision, n); i += n;
        n = sizeof(size_t); memcpy(packed->data()+i, &transferSize, n); i += n;
        return packed;
    }

    JobSignature& deserialize(const std::vector<uint8_t>& packed) override {
        int i = 0, n;
        n = sizeof(int);    memcpy(&jobId, packed.data()+i, n); i += n;
        n = sizeof(int);    memcpy(&rootRank, packed.data()+i, n); i += n;
        n = sizeof(int);    memcpy(&revision, packed.data()+i, n); i += n;
        n = sizeof(size_t); memcpy(&transferSize, packed.data()+i, n); i += n;
        return *this;
    }
};

struct JobMessage : public Serializable {

    int jobId;
    int tag;
    int epoch;
    std::vector<int> payload;

public:
    std::shared_ptr<std::vector<uint8_t>> serialize() const override {
        int size = 3*sizeof(int) + payload.size()*sizeof(int);
        std::shared_ptr<std::vector<uint8_t>> packed = std::make_shared<std::vector<uint8_t>>(size);

        int i = 0, n;
        n = sizeof(int); memcpy(packed->data()+i, &jobId, n); i += n;
        n = sizeof(int); memcpy(packed->data()+i, &tag, n); i += n;
        n = sizeof(int); memcpy(packed->data()+i, &epoch, n); i += n;
        n = payload.size()*sizeof(int); memcpy(packed->data()+i, payload.data(), n); i += n;
        return packed;
    }

    JobMessage& deserialize(const std::vector<uint8_t>& packed) override {
        int i = 0, n;
        n = sizeof(int); memcpy(&jobId, packed.data()+i, n); i += n;
        n = sizeof(int); memcpy(&tag, packed.data()+i, n); i += n;
        n = sizeof(int); memcpy(&epoch, packed.data()+i, n); i += n;
        n = packed.size()-i; payload.resize(n/sizeof(int)); 
        memcpy(payload.data(), packed.data()+i, n); i += n;
        return *this;
    }
};

struct IntPair : public Serializable {

    int first;
    int second;

public:
    IntPair(int first, int second) : first(first), second(second) {}

    IntPair(const std::vector<uint8_t>& packed) {
        deserialize(packed);
    }

    std::shared_ptr<std::vector<uint8_t>> serialize() const override {
        int size = (2*sizeof(int));
        std::shared_ptr<std::vector<uint8_t>> packed = std::make_shared<std::vector<uint8_t>>(size);
        int i = 0, n;
        n = sizeof(int); memcpy(packed->data()+i, &first, n); i += n;
        n = sizeof(int); memcpy(packed->data()+i, &second, n); i += n;
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
    IntVec(const std::vector<int>& data) : data(data) {}
    IntVec(const std::initializer_list<int>& list) : data(list) {}
    IntVec(const std::vector<uint8_t>& packed) {
        deserialize(packed);
    }

    std::shared_ptr<std::vector<uint8_t>> serialize() const override {
        int size = (data.size()*sizeof(int));
        std::shared_ptr<std::vector<uint8_t>> packed = std::make_shared<std::vector<uint8_t>>(size);
        memcpy(packed->data(), data.data(), size);
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
