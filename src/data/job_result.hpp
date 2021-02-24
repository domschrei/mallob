
#ifndef DOMPASCH_MALLOB_JOB_RESULT_H
#define DOMPASCH_MALLOB_JOB_RESULT_H

#include <memory>
#include <vector>
#include <cstring>

#include "serializable.hpp"

struct JobResult : public Serializable {

    int id = 0;
    int revision;
    int result;
    std::vector<int> solution;

public:
    JobResult() {}
    JobResult(int id, int result, std::vector<int> solution) : id(id), result(result), solution(solution) {}

    int getTransferSize() const {return sizeof(int)*3 + sizeof(int)*solution.size();}

    JobResult& deserialize(const std::vector<uint8_t>& packed) override;
    std::vector<uint8_t> serialize() const override;
};

#endif