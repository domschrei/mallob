
#pragma once

#include "data/job_transfer.hpp"
#include "data/serializable.hpp"

struct QbfNotification : public Serializable {
    int rootJobId;
    int parentJobId;
    int depth;
    int childIdx;
    int resultCode;

    QbfNotification() {}
    QbfNotification(int rootJobId, int parentJobId, int depth, int childIdx, int resultCode) :
        rootJobId(rootJobId),
        parentJobId(parentJobId),
        depth(depth),
        childIdx(childIdx),
        resultCode(resultCode) {}

    virtual std::vector<uint8_t> serialize() const override {
        return IntVec({rootJobId, parentJobId, depth, childIdx, resultCode}).serialize();
    }
    virtual QbfNotification& deserialize(const std::vector<uint8_t>& packed) override {
        IntVec vec = Serializable::get<IntVec>(packed);
        rootJobId = vec[0];
        parentJobId = vec[1];
        depth = vec[2];
        childIdx = vec[3];
        resultCode = vec[4];
        return *this;
    }
};
