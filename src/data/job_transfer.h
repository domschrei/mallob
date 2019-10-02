
#ifndef DOMPASCH_JOB_TRANSFER
#define DOMPASCH_JOB_TRANSFER

#include <vector>
#include <cstring>

#include "serializable.h"

/**
 * Sent around during the search of a node to adopt the job.
 */
struct JobRequest : public Serializable  {

    int jobId;
    int rootRank;
    int requestingNodeRank;
    int requestedNodeIndex;
    int fullTransfer;
    int iteration;
    int numHops;

public:
    JobRequest() = default;

    JobRequest(int jobId, int rootRank, int requestingNodeRank, int requestedNodeIndex, int iteration, int numHops) :
        jobId(jobId),
        rootRank(rootRank),
        requestingNodeRank(requestingNodeRank),
        requestedNodeIndex(requestedNodeIndex),
        fullTransfer(1),
        iteration(iteration),
        numHops(numHops) {}

    std::vector<int> serialize() const override {
        std::vector<int> packed;
        packed.push_back(jobId);
        packed.push_back(rootRank);
        packed.push_back(requestingNodeRank);
        packed.push_back(requestedNodeIndex);
        packed.push_back(fullTransfer);
        packed.push_back(iteration);
        packed.push_back(numHops);
        return packed;
    }

    void deserialize(const std::vector<int> &packed) override {
        int i = 0;
        jobId = packed[i++];
        rootRank = packed[i++];
        requestingNodeRank = packed[i++];
        requestedNodeIndex = packed[i++];
        fullTransfer = packed[i++];
        iteration = packed[i++];
        numHops = packed[i++];
    }
};

/**
 * Sent as pre-information on a job that will be transferred
 * based on a previous commitment.
 */
struct JobSignature : public Serializable {

    int jobId;
    int rootRank;
    int formulaSize;
    int assumptionsSize;

public:
    JobSignature() = default;

    JobSignature(int jobId, int rootRank, int formulaSize, int assumptionsSize) :
        jobId(jobId),
        rootRank(rootRank),
        formulaSize(formulaSize),
        assumptionsSize(assumptionsSize) {}

    int getTransferSize() const {
        // 3 meta data ints, formula size, separator, assumptions, closing zero
        return 3 + formulaSize + 1 + assumptionsSize + 1;
    }

    std::vector<int> serialize() const override {
        std::vector<int> packed;
        packed.push_back(jobId);
        packed.push_back(rootRank);
        packed.push_back(formulaSize);
        packed.push_back(assumptionsSize);
        return packed;
    }

    void deserialize(const std::vector<int>& packed) override {
        int i = 0;
        jobId = packed[i++];
        rootRank = packed[i++];
        formulaSize = packed[i++];
        assumptionsSize = packed[i++];
    }
};

#endif
