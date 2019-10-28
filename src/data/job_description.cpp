
#include "job_description.h"

#include "util/console.h"

int JobDescription::getTransferSize() const {
    return 2*sizeof(int)
            +sizeof(float)
            +payload.size()*sizeof(int);
}

std::vector<uint8_t> JobDescription::serialize() const {

    std::vector<uint8_t> packed(getTransferSize());

    // Basic data
    int i = 0, n;
    n = sizeof(int); memcpy(packed.data()+i, &id, n); i += n;
    n = sizeof(int); memcpy(packed.data()+i, &rootRank, n); i += n;
    n = sizeof(float); memcpy(packed.data()+i, &priority, n); i += n;

    // Clauses
    n = payload.size()*sizeof(int); memcpy(packed.data()+i, payload.data(), n); i += n;

    return packed;
}

void JobDescription::deserialize(const std::vector<uint8_t>& packed) {

    int i = 0, n;

    // Basic data
    n = sizeof(int); memcpy(&id, packed.data()+i, n); i += n;
    n = sizeof(int); memcpy(&rootRank, packed.data()+i, n); i += n;
    n = sizeof(float); memcpy(&priority, packed.data()+i, n); i += n;

    // Clauses
    n = packed.size()-i; payload.resize(n/sizeof(int)); 
    Console::log(Console::VVVERB, "%i %i %i", i, n, payload.size());
    memcpy(payload.data(), packed.data()+i, n); i += n;
}

std::vector<uint8_t> JobResult::serialize() const {

    std::vector<uint8_t> packed(2*sizeof(int) + solution.size()*sizeof(int));

    int i = 0, n;
    n = sizeof(int); memcpy(packed.data()+i, &id, n); i += n;
    n = sizeof(int); memcpy(packed.data()+i, &result, n); i += n;
    n = solution.size() * sizeof(int); memcpy(packed.data()+i, solution.data(), n); i += n;
    return packed;
}

void JobResult::deserialize(const std::vector<uint8_t>& packed) {

    int i = 0, n;
    n = sizeof(int); memcpy(&id, packed.data()+i, n); i += n;
    n = sizeof(int); memcpy(&result, packed.data()+i, n); i += n;
    n = packed.size()-i; solution.resize(n/sizeof(int));
    memcpy(solution.data(), packed.data()+i, n); i += n;
}