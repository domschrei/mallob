
#pragma once

#include "app/sat/proof/trusted/trusted_utils.hpp"
#include "data/serializable.hpp"
#include "util/logger.hpp"
#include "util/string_utils.hpp"

struct Witness : public Serializable {
    int cidx = 0;
    int result = 0;
    char data[SIG_SIZE_BYTES];
    std::vector<int> asmpt;

    bool valid() const {return cidx != 0 || result != 0;}
    virtual std::vector<uint8_t> serialize() const {
        std::vector<uint8_t> out(sizeof(int)*2 + SIG_SIZE_BYTES + sizeof(int)*asmpt.size());
        int i = 0, n;
        n = sizeof(int); memcpy(out.data()+i, &cidx, n); i += n;
        n = sizeof(int); memcpy(out.data()+i, &result, n); i += n;
        n = SIG_SIZE_BYTES; memcpy(out.data()+i, data, n); i += n;
        n = sizeof(int)*asmpt.size(); memcpy(out.data()+i, asmpt.data(), n); i += n;
        return out;
    }
    virtual Serializable& deserialize(const std::vector<uint8_t>& in) {
        int i = 0, n;
        n = sizeof(int); memcpy(&cidx, in.data()+i, n); i += n;
        n = sizeof(int); memcpy(&result, in.data()+i, n); i += n;
        n = SIG_SIZE_BYTES; memcpy(data, in.data()+i, n); i += n;
        int remainingBytes = in.size() - i;
        int asmptSize = remainingBytes / sizeof(int);
        asmpt.resize(asmptSize);
        n = remainingBytes; memcpy(asmpt.data(), in.data()+i, n); i += n;
        assert(i == in.size());
        return *this;
    }

    std::string toStr() const {
        return std::to_string(cidx) + " " + std::to_string(result) + " "
            + Logger::dataToHexStr((const uint8_t*) data, SIG_SIZE_BYTES)
            + " " + StringUtils::getSummary(asmpt, INT32_MAX);
    }

    void appendToSolutionVector(std::vector<int>& sol) const {
        auto wPacked = serialize();
        sol.push_back(INT32_MAX);
        sol.insert(sol.end(), (int*) wPacked.data(), (int*) (wPacked.data()+wPacked.size()));
        sol.push_back(wPacked.size());
        sol.push_back(INT32_MAX);
    }
    static Witness extractWitnessFromSolutionVector(std::vector<int>& sol) {
        Witness w;
        if (sol.empty() || sol.back() != INT32_MAX) return w;
        sol.pop_back(); // INT32_MAX
        if (sol.empty()) return w;
        int packedWitnessSizeBytes = sol.back();
        sol.pop_back(); // size
        assert(sol.size() >= 1 + packedWitnessSizeBytes / sizeof(int));
        int solBeginWitness = sol.size() - packedWitnessSizeBytes / sizeof(int);
        assert(sol[solBeginWitness-1] == INT32_MAX);
        std::vector<uint8_t> packed(
            (uint8_t*) (sol.data()+solBeginWitness),
            (uint8_t*) (sol.data()+sol.size())
        );
        assert(packed.size() == packedWitnessSizeBytes || log_return_false("%i != %i\n", packed.size(), packedWitnessSizeBytes));
        w.deserialize(packed);
        sol.resize(solBeginWitness - 1); // also remove INT32_MAX before witness begins
        return w;
    }
};
