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

    JobDescription() : id(-1), rootRank(-1), priority(-1) {}
    JobDescription(int id, float priority) : id(id), rootRank(-1), priority(priority) {}

    int getId() const {return id;};
    int getRootRank() const {return rootRank;};
    float getPriority() const {return priority;};
    int getPayloadSize() const {
        return payload.size() + 1;
    };
    const std::vector<int>& getPayload() const {return payload;};
    float getArrival() const {return arrival;};

    void setRootRank(int rootRank) {this->rootRank = rootRank;}
    void setPayload(const std::vector<int>& payload) {this->payload = payload;};
    void setArrival(float arrival) {this->arrival = arrival;};

    void deserialize(const std::vector<int>& packed) override;
    std::vector<int> serialize() const override;
};

struct JobResult : public Serializable {

    int id;
    int result;
    std::vector<int> solution;

public:
    JobResult() : id(-1), result(0), solution(std::vector<int>()) {}
    JobResult(int id, int result, std::vector<int> solution) : id(id), result(result), solution(solution) {}

    int getTransferSize() const {return 2 + solution.size();};

    void deserialize(const std::vector<int>& packed) override;
    std::vector<int> serialize() const override;

};

#endif /* end of include guard: DOMPASCH_CUCKOO_REBALANCER_JOB */
