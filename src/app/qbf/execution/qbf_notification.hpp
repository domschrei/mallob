
#pragma once

#include "data/job_transfer.hpp"
#include "data/serializable.hpp"

struct QbfNotification : public Serializable {
    int rootJobId;
    int depth;
    int resultCode;

    QbfNotification() {}
    QbfNotification(int rootJobId, int depth, int resultCode) :
        rootJobId(rootJobId),
        depth(depth),
        resultCode(resultCode) {}

    virtual std::vector<uint8_t> serialize() const override {
        return IntVec({rootJobId, depth, resultCode}).serialize();
    }
    virtual QbfNotification& deserialize(const std::vector<uint8_t>& packed) override {
        IntVec vec = Serializable::get<IntVec>(packed);
        rootJobId = vec[0];
        depth = vec[1];
        resultCode = vec[2];
        return *this;
    }
};
