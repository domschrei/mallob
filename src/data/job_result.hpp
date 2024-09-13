
#ifndef DOMPASCH_MALLOB_JOB_RESULT_H
#define DOMPASCH_MALLOB_JOB_RESULT_H

#include <assert.h>
#include <vector>
#include <cstring>
#include <cstdint>
#include <utility>

#include "serializable.hpp"
#include "util/assert.hpp"

struct JobResult : public Serializable {

    int id = 0;
    int revision;
    int result;
    enum EncodedType {INT, FLOAT} encodedType = INT;
    int winningInstanceId;
    unsigned long globalStartOfSuccessEpoch;

private:
    std::vector<int> solution;
    std::vector<uint8_t> packedData;

public:
    JobResult() {}
    JobResult(std::vector<uint8_t>&& packedData);
    virtual ~JobResult() {}

    JobResult(JobResult&& other) {
        *this = std::move(other);
    }
    JobResult& operator=(JobResult&& other) {
        id = other.id;
        revision = other.revision;
        result = other.result;
        encodedType = other.encodedType;
        winningInstanceId = other.winningInstanceId;
        globalStartOfSuccessEpoch = other.globalStartOfSuccessEpoch;
        solution = std::move(other.solution);
        packedData = std::move(other.packedData);
        other.id = 0;
        return *this;
    }

    JobResult& deserialize(const std::vector<uint8_t>& packed) override;

    std::vector<uint8_t> serialize() const override;
    std::vector<uint8_t>&& moveSerialization();
    void updateSerialization();

    void setSolution(std::vector<int>&& solution);
    void setSolutionToSerialize(const int* begin, size_t size);

    bool hasSerialization() const {
        return packedData.size() >= 3*sizeof(int) + sizeof(EncodedType);
    }
    size_t getSolutionSize() const;
    inline int getSolution(size_t pos) const {
        assert(pos < getSolutionSize());
        static_assert(sizeof(int) == sizeof(EncodedType));
        if (!packedData.empty()) {
            return *(
                (int*) (packedData.data() + (4+pos)*sizeof(int))
            );
        }
        return solution[pos];
    }

    std::vector<int> extractSolution();
    std::vector<int> copySolution() const;
};

#endif