
#pragma once

#include "data/job_transfer.hpp"
#include "data/serializable.hpp"

struct SubjobReadyMsg : public Serializable {
    int rootJobId;
    int depth;
    int childIdx;
    int childJobId;

    SubjobReadyMsg() {}
    SubjobReadyMsg(int rootJobId, int depth, int childIdx, int childJobId) :
        rootJobId(rootJobId),
        depth(depth),
        childIdx(childIdx),
        childJobId(childJobId) {}

    virtual std::vector<uint8_t> serialize() const override {
        return IntVec({rootJobId, depth, childIdx, childJobId}).serialize();
    }
    virtual SubjobReadyMsg& deserialize(const std::vector<uint8_t>& packed) override {
        IntVec vec = Serializable::get<IntVec>(packed);
        rootJobId = vec[0];
        depth = vec[1];
        childIdx = vec[2];
        childJobId = vec[3];
        return *this;
    }
};
