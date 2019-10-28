
#ifndef DOMPASCH_JOB_TRANSFER
#define DOMPASCH_JOB_TRANSFER

#include <vector>
#include <cstring>

#include "serializable.h"

/**
 * Sent around during the search of a node to adopt the job.
 */
struct JobRequest : public Serializable {

    int jobId;
    int rootRank;
    int requestingNodeRank;
    int requestedNodeIndex;
    int fullTransfer;
    int epoch;
    int numHops;

public:
    JobRequest() = default;

    JobRequest(int jobId, int rootRank, int requestingNodeRank, int requestedNodeIndex, int epoch, int numHops) :
        jobId(jobId),
        rootRank(rootRank),
        requestingNodeRank(requestingNodeRank),
        requestedNodeIndex(requestedNodeIndex),
        fullTransfer(1),
        epoch(epoch),
        numHops(numHops) {}

    std::vector<uint8_t> serialize() const override {
        std::vector<uint8_t> packed(7*sizeof(int));

        int i = 0, n;
        n = sizeof(int); memcpy(packed.data()+i, &jobId, n); i += n;
        n = sizeof(int); memcpy(packed.data()+i, &rootRank, n); i += n;
        n = sizeof(int); memcpy(packed.data()+i, &requestingNodeRank, n); i += n;
        n = sizeof(int); memcpy(packed.data()+i, &requestedNodeIndex, n); i += n;
        n = sizeof(int); memcpy(packed.data()+i, &fullTransfer, n); i += n;
        n = sizeof(int); memcpy(packed.data()+i, &epoch, n); i += n;
        n = sizeof(int); memcpy(packed.data()+i, &numHops, n); i += n;
        return packed;
    }

    void deserialize(const std::vector<uint8_t> &packed) override {
        int i = 0, n;
        n = sizeof(int); memcpy(&jobId, packed.data()+i, n); i += n;
        n = sizeof(int); memcpy(&rootRank, packed.data()+i, n); i += n;
        n = sizeof(int); memcpy(&requestingNodeRank, packed.data()+i, n); i += n;
        n = sizeof(int); memcpy(&requestedNodeIndex, packed.data()+i, n); i += n;
        n = sizeof(int); memcpy(&fullTransfer, packed.data()+i, n); i += n;
        n = sizeof(int); memcpy(&epoch, packed.data()+i, n); i += n;
        n = sizeof(int); memcpy(&numHops, packed.data()+i, n); i += n;
    }
};

/**
 * Sent as pre-information on a job that will be transferred
 * based on a previous commitment.
 */
struct JobSignature : public Serializable {

    int jobId;
    int rootRank;
    size_t transferSize;

public:
    JobSignature() = default;

    JobSignature(int jobId, int rootRank, size_t transferSize) :
        jobId(jobId),
        rootRank(rootRank),
        transferSize(transferSize) {}

    int getTransferSize() const {
        return transferSize;
    }

    std::vector<uint8_t> serialize() const override {
        std::vector<uint8_t> packed(2*sizeof(int) + sizeof(size_t));

        int i = 0, n;
        n = sizeof(int); memcpy(packed.data()+i, &jobId, n); i += n;
        n = sizeof(int); memcpy(packed.data()+i, &rootRank, n); i += n;
        n = sizeof(size_t); memcpy(packed.data()+i, &transferSize, n); i += n;
        return packed;
    }

    void deserialize(const std::vector<uint8_t>& packed) override {
        int i = 0, n;
        n = sizeof(int); memcpy(&jobId, packed.data()+i, n); i += n;
        n = sizeof(int); memcpy(&rootRank, packed.data()+i, n); i += n;
        n = sizeof(size_t); memcpy(&transferSize, packed.data()+i, n); i += n;
    }
};

struct JobMessage : public Serializable {

    int jobId;
    int tag;
    int epoch;
    std::vector<int> payload;

public:
    std::vector<uint8_t> serialize() const override {
        std::vector<uint8_t> packed(3*sizeof(int) + payload.size()*sizeof(int));

        int i = 0, n;
        n = sizeof(int); memcpy(packed.data()+i, &jobId, n); i += n;
        n = sizeof(int); memcpy(packed.data()+i, &tag, n); i += n;
        n = sizeof(int); memcpy(packed.data()+i, &epoch, n); i += n;
        n = payload.size()*sizeof(int); memcpy(packed.data()+i, payload.data(), n); i += n;
        return packed;
    }

    void deserialize(const std::vector<uint8_t>& packed) override {
        int i = 0, n;
        n = sizeof(int); memcpy(&jobId, packed.data()+i, n); i += n;
        n = sizeof(int); memcpy(&tag, packed.data()+i, n); i += n;
        n = sizeof(int); memcpy(&epoch, packed.data()+i, n); i += n;
        n = packed.size()-i; payload.resize(n/sizeof(int)); 
        memcpy(payload.data(), packed.data()+i, n); i += n;
    }
};

struct IntPair : public Serializable {

    int first;
    int second;

public:
    IntPair(int first, int second) : first(first), second(second) {}

    IntPair(std::vector<uint8_t>& packed) {
        deserialize(packed);
    }

    std::vector<uint8_t> serialize() const override {
        std::vector<uint8_t> packed(2*sizeof(int));
        int i = 0, n;
        n = sizeof(int); memcpy(packed.data()+i, &first, n); i += n;
        n = sizeof(int); memcpy(packed.data()+i, &second, n); i += n;
        return packed;
    }

    void deserialize(const std::vector<uint8_t>& packed) override {
        int i = 0, n;
        n = sizeof(int); memcpy(&first, packed.data()+i, n); i += n;
        n = sizeof(int); memcpy(&second, packed.data()+i, n); i += n;
    }
};

#endif
