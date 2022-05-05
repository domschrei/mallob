
#include "job_result.hpp"

#include "util/assert.hpp"
#include "util/logger.hpp"

JobResult::JobResult(std::vector<uint8_t>&& packedData) : packedData(std::move(packedData)) {
    int i = 0, n;
    n = sizeof(int); memcpy(&id, this->packedData.data()+i, n); i += n;
    n = sizeof(int); memcpy(&result, this->packedData.data()+i, n); i += n;
    n = sizeof(int); memcpy(&revision, this->packedData.data()+i, n); i += n;
    n = sizeof(EncodedType); memcpy(&encodedType, this->packedData.data()+i, n); i += n;
}

std::vector<uint8_t>&& JobResult::moveSerialization() {
    return std::move(packedData);
}

std::vector<uint8_t> JobResult::serialize() const {
    
    int size = 3*sizeof(int) + sizeof(EncodedType) + solution.size()*sizeof(int);
    std::vector<uint8_t> packed(size);

    int i = 0, n;
    n = sizeof(int); memcpy(packed.data()+i, &id, n); i += n;
    n = sizeof(int); memcpy(packed.data()+i, &result, n); i += n;
    n = sizeof(int); memcpy(packed.data()+i, &revision, n); i += n;
    n = sizeof(EncodedType); memcpy(packed.data()+i, &encodedType, n); i += n;
    n = solution.size() * sizeof(int); memcpy(packed.data()+i, solution.data(), n); i += n;
    return packed;
}

void JobResult::updateSerialization() {
    int i = 0, n;
    assert(packedData.size() >= 3*sizeof(int) + sizeof(EncodedType));
    n = sizeof(int); memcpy(packedData.data()+i, &id, n); i += n;
    n = sizeof(int); memcpy(packedData.data()+i, &result, n); i += n;
    n = sizeof(int); memcpy(packedData.data()+i, &revision, n); i += n;
    n = sizeof(EncodedType); memcpy(packedData.data()+i, &encodedType, n); i += n;
}

JobResult& JobResult::deserialize(const std::vector<uint8_t>& packed) {

    int i = 0, n;
    n = sizeof(int); memcpy(&id, packed.data()+i, n); i += n;
    n = sizeof(int); memcpy(&result, packed.data()+i, n); i += n;
    n = sizeof(int); memcpy(&revision, packed.data()+i, n); i += n;
    n = sizeof(EncodedType); memcpy(&encodedType, packed.data()+i, n); i += n;
    n = packed.size()-i; solution.resize(n/sizeof(int));
    memcpy(solution.data(), packed.data()+i, n); i += n;
    return *this;
}

void JobResult::setSolution(std::vector<int>&& solution) {
    this->solution = std::move(solution); 
}

void JobResult::setSolutionToSerialize(const int* solutionPtr, size_t solutionSize) {
    int size = 3*sizeof(int) + sizeof(EncodedType) + solutionSize*sizeof(int);
    packedData.resize(size);

    int i = 0, n;
    n = sizeof(int); memcpy(packedData.data()+i, &id, n); i += n;
    n = sizeof(int); memcpy(packedData.data()+i, &result, n); i += n;
    n = sizeof(int); memcpy(packedData.data()+i, &revision, n); i += n;
    n = sizeof(EncodedType); memcpy(packedData.data()+i, &encodedType, n); i += n;
    n = solutionSize * sizeof(int); memcpy(packedData.data()+i, solutionPtr, n); i += n;
}

size_t JobResult::getSolutionSize() const {
    static_assert(sizeof(int) == sizeof(EncodedType));
    if (!packedData.empty()) return packedData.size()/sizeof(int) - 4;
    return solution.size();
}

std::vector<int> JobResult::extractSolution() {
    if (!packedData.empty()) {
        JobResult res; res.deserialize(packedData);
        return res.solution;
    }
    return std::vector<int>(std::move(solution));
}
