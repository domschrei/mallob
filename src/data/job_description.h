#ifndef DOMPASCH_CUCKOO_REBALANCER_JOB
#define DOMPASCH_CUCKOO_REBALANCER_JOB

#include <vector>
#include <cstring>

#include "serializable.h"

/**
 * The actual job structure, containing the full description.
 */
class JobDescription : public Serializable {

private:

    // Global meta data
    int id;
    int rootRank;
    float priority;

    float arrival; // only for introducing a job

    // Payload (logic to solve)
    std::vector<int> payload; 

public:

    JobDescription() = default;
    JobDescription(int id, float priority) : id(id), priority(priority) {}

    int getId() const {return id;};
    int getRootRank() const {return rootRank;};
    float getPriority() const {return priority;};
    int getTransferSize() const;
    const std::vector<int>& getPayload() const {return payload;};
    float getArrival() const {return arrival;};

    void setRootRank(int rootRank) {this->rootRank = rootRank;}
    void setPayload(const std::vector<int>& payload) {this->payload = payload;};
    void setArrival(float arrival) {this->arrival = arrival;};

    void deserialize(const std::vector<uint8_t>& packed) override;
    std::shared_ptr<std::vector<uint8_t>> serialize() const override;
};

struct JobResult : public Serializable {

    int id;
    int result;
    std::vector<int> solution;

public:
    JobResult() : solution(std::vector<int>()) {}
    JobResult(int id, int result, std::vector<int> solution) : id(id), result(result), solution(solution) {}

    int getTransferSize() const {return sizeof(int) + sizeof(int) + sizeof(int)*solution.size();};

    void deserialize(const std::vector<uint8_t>& packed) override;
    std::shared_ptr<std::vector<uint8_t>> serialize() const override;

};

#endif /* end of include guard: DOMPASCH_CUCKOO_REBALANCER_JOB */
