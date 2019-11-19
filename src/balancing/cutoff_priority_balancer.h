

#ifndef DOMPASCH_BALANCER_CUTOFF_PRIORITY_H
#define DOMPASCH_BALANCER_CUTOFF_PRIORITY_H

#include <utility>

#include "balancing/balancer.h"
#include "data/reduceable.h"
#include "util/console.h"

#define ITERATIVE_ROUNDING false

class PriorityComparator {
private:
    std::map<int, Job*>& _jobs;
public:
    PriorityComparator(std::map<int, Job*>& jobs) : _jobs(jobs) {}
    bool operator() (const int& lhs, const int& rhs) const {
        return _jobs[lhs]->getDescription().getPriority() > _jobs[rhs]->getDescription().getPriority();
    }
};

struct ResourcesInfo : public Reduceable {

float assignedResources;
std::vector<float> priorities;
std::vector<float> demandedResources;

ResourcesInfo() :
    assignedResources(0),
    priorities(std::vector<float>()),
    demandedResources(std::vector<float>()) {}

void merge(const Reduceable& other) override {
    const ResourcesInfo& info = (ResourcesInfo&) other;
    assignedResources += info.assignedResources;
    for (size_t i = 0; i < info.priorities.size(); i++) {

        float prioToInsert = info.priorities[i];

        // Find insertion point
        size_t idx = 0;
        while (idx < priorities.size() && priorities[idx] > prioToInsert) idx++;

        // If this priority did not exist before,
        // insert an element into both structures
        if (idx >= priorities.size() || priorities[idx] != prioToInsert) {
            priorities.insert(priorities.begin()+idx, prioToInsert);
            demandedResources.insert(demandedResources.begin()+idx, 0);
        }
        // Update demanded resources
        demandedResources[idx] += info.demandedResources[i];
    }
}

bool isEmpty() override {
    return assignedResources < 0.0001f && priorities.empty() && demandedResources.empty();
}

std::shared_ptr<std::vector<uint8_t>> serialize() const override {
    int size = (sizeof(float)+priorities.size()*sizeof(float)+demandedResources.size()*sizeof(float));
    std::shared_ptr<std::vector<uint8_t>> data = std::make_shared<std::vector<uint8_t>>(size);
    int i = 0, n;
    n = sizeof(float); memcpy(data->data()+i, &assignedResources, n); i += n;
    n = priorities.size() * sizeof(float); memcpy(data->data()+i, priorities.data(), n); i += n;
    n = demandedResources.size() * sizeof(float); memcpy(data->data()+i, demandedResources.data(), n); i += n;
    return data;
}
void deserialize(const std::vector<uint8_t>& packed) override {
    int i = 0, n;
    n = sizeof(float); memcpy(&assignedResources, packed.data()+i, n); i += n;
    int size = (packed.size() - i) / 2;
    assert(size >= 0);
    priorities.clear(); priorities.resize(size / sizeof(float));
    n = size; memcpy(priorities.data(), packed.data()+i, n); i += n;
    demandedResources.clear(); demandedResources.resize(size / sizeof(float));
    n = size; memcpy(demandedResources.data(), packed.data()+i, n); i += n;
}
std::unique_ptr<Reduceable> getDeserialized(const std::vector<uint8_t>& packed) const override {
    std::unique_ptr<Reduceable> out(new ResourcesInfo());
    out->deserialize(packed);
    return out;
}
};

struct SortedDoubleSequence : public Reduceable {
    std::vector<double> data;

SortedDoubleSequence() : data() {}
void add(double x) {
    int i = 0;
    while (i < data.size() && data[i] < x) i++;
    data.insert(data.begin() + i, x);
}
int size() {return data.size();}
double& operator[](int i) {return data[i];}
void merge(const Reduceable& other) override {
    const SortedDoubleSequence& otherSet = (SortedDoubleSequence&) other;
    int i = 0, j = 0, x = 0;
    std::vector<double> newData(data.size() + otherSet.data.size());
    for (int x = 0; x < data.size() + otherSet.data.size(); x++) {
        if (i < data.size() && (j >= otherSet.data.size() || data[i] <= otherSet.data[j]))
            newData[x] = data[i++];
        else
            newData[x] = data[j++];
    }
    this->data = newData;
}
bool isEmpty() override {
    return data.empty();
}
std::shared_ptr<std::vector<uint8_t>> serialize() const override {
    int size = this->data.size()*sizeof(double);
    std::shared_ptr<std::vector<uint8_t>> data = std::make_shared<std::vector<uint8_t>>(size);
    memcpy(data->data(), this->data.data(), size);
    return data;
}
void deserialize(const std::vector<uint8_t>& packed) override {
    int size = packed.size() / sizeof(double);
    this->data.clear(); this->data.resize(size);
    memcpy(this->data.data(), packed.data(), packed.size());
}
std::unique_ptr<Reduceable> getDeserialized(const std::vector<uint8_t>& packed) const override {
    std::unique_ptr<Reduceable> out(new SortedDoubleSequence());
    out->deserialize(packed);
    return out;
}
};

enum BalancingStage {
    INITIAL_DEMAND, REDUCE_RESOURCES, BROADCAST_RESOURCES, REDUCE_REMAINDERS, BROADCAST_REMAINDERS, GLOBAL_ROUNDING
};

class CutoffPriorityBalancer : public Balancer {

public:
    CutoffPriorityBalancer(MPI_Comm& comm, Parameters& params, Statistics& stats) : Balancer(comm, params, stats), _local_jobs(NULL) {

    }
    std::map<int, int> balance(std::map<int, Job*>& jobs) override;

    bool beginBalancing(std::map<int, Job*>& jobs) override;
    bool canContinueBalancing() override;
    bool continueBalancing() override;
    bool continueBalancing(MessageHandlePtr handle) override;
    std::map<int, int> getBalancingResult() override;

    bool finishResourcesReduction();
    bool finishRemaindersReduction();
    bool continueRoundingUntilReduction(int lower, int upper);
    bool continueRoundingFromReduction();
    bool finishRounding();

private:
    std::set<int, PriorityComparator>* _local_jobs;
    BalancingStage _stage;

    ResourcesInfo _resources_info;
    SortedDoubleSequence _remainders;
    int _lower_remainder_idx;
    int _upper_remainder_idx;

    int _total_volume;
    std::map<int, float> _assignments;
    std::map<int, int> _rounded_assignments;

};

#endif
