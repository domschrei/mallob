
#pragma once

#include "data/serializable.hpp"
#include "util/assert.hpp"
#include <cstring>

struct ClientDirective : public Serializable {

    int jobId {-1};
    enum DirectiveType {
        NONE, SHRINK
    } type {NONE};
    uint8_t data[16];

    std::vector<uint8_t> serialize() const override {
        std::vector<uint8_t> packed(sizeof(jobId) + sizeof(type) + 16);
        int n, i = 0;
        n = sizeof(jobId); memcpy(packed.data()+i, &jobId, n); i += n;
        n = sizeof(type); memcpy(packed.data()+i, &type, n); i += n;
        n = 16; memcpy(packed.data()+i, data, n); i += n;
        assert(i == packed.size());
        return packed;
    }

    ClientDirective& deserialize(const std::vector<uint8_t> &packed) override {
        int n, i = 0;
        n = sizeof(jobId); memcpy(&jobId, packed.data()+i, n); i += n;
        n = sizeof(type); memcpy(&type, packed.data()+i, n); i += n;
        n = 16; memcpy(data, packed.data()+i, n); i += n;
        assert(i == sizeof(jobId) + sizeof(type) + 16);
        return *this;
    }

    void setData(float f) {
        memcpy(data, &f, sizeof(float));
    }
    float getDataAsFloat() const {
        float f;
        memcpy(&f, data, sizeof(float));
        return f;
    }
};
