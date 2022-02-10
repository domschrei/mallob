
#include "job_result.hpp"

#include "util/assert.hpp"

JobResult::JobResult(std::vector<uint8_t>&& packedData) : packedData(std::move(packedData)) {
    int i = 0, n;
    n = sizeof(int); memcpy(&id, this->packedData.data()+i, n); i += n;
    n = sizeof(int); memcpy(&result, this->packedData.data()+i, n); i += n;
    n = sizeof(int); memcpy(&revision, this->packedData.data()+i, n); i += n;
}

std::vector<uint8_t> JobResult::serialize() const {
    int size = 3*sizeof(int) + solution.size()*sizeof(int);
    std::vector<uint8_t> packed(size);

    int i = 0, n;
    n = sizeof(int); memcpy(packed.data()+i, &id, n); i += n;
    n = sizeof(int); memcpy(packed.data()+i, &result, n); i += n;
    n = sizeof(int); memcpy(packed.data()+i, &revision, n); i += n;
    n = solution.size() * sizeof(int); memcpy(packed.data()+i, solution.data(), n); i += n;
    return packed;
}

JobResult& JobResult::deserialize(const std::vector<uint8_t>& packed) {

    int i = 0, n;
    n = sizeof(int); memcpy(&id, packed.data()+i, n); i += n;
    n = sizeof(int); memcpy(&result, packed.data()+i, n); i += n;
    n = sizeof(int); memcpy(&revision, packed.data()+i, n); i += n;
    n = packed.size()-i; solution.resize(n/sizeof(int));
    memcpy(solution.data(), packed.data()+i, n); i += n;
    return *this;
}

void JobResult::setSolution(std::vector<int>&& solution) {
    this->solution = std::move(solution); 
}

size_t JobResult::getSolutionSize() const {
    if (!packedData.empty()) return packedData.size()/sizeof(int) - 3;
    return solution.size();
}

std::vector<int> JobResult::extractSolution() {
    if (!packedData.empty()) {
        JobResult res; res.deserialize(packedData);
        return res.solution;
    }
    return std::vector<int>(std::move(solution));
}
