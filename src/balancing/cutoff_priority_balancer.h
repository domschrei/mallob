

#ifndef DOMPASCH_BALANCER_CUTOFF_PRIORITY_H
#define DOMPASCH_BALANCER_CUTOFF_PRIORITY_H

#include <utility>

#include "balancing/balancer.h"
#include "data/reduceable.h"
#include "util/console.h"

class PriorityComparator {
private:
    std::map<int, Job*>& jobs;
public:
    PriorityComparator(std::map<int, Job*>& jobs) : jobs(jobs) {}
    bool operator() (const int& lhs, const int& rhs) const {
        return jobs[lhs]->getDescription().getPriority() > jobs[rhs]->getDescription().getPriority();
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

std::vector<uint8_t> serialize() const override {
    std::vector<uint8_t> data(sizeof(float)+priorities.size()*sizeof(float)+demandedResources.size()*sizeof(float));

    int i = 0, n;
    n = sizeof(float); memcpy(data.data()+i, &assignedResources, n); i += n;
    n = priorities.size() * sizeof(float); memcpy(data.data()+i, priorities.data(), n); i += n;
    n = demandedResources.size() * sizeof(float); memcpy(data.data()+i, demandedResources.data(), n); i += n;
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

enum BalancingStage {
    INITIAL_DEMAND, REDUCE_RESOURCES, BROADCAST_RESOURCES, GLOBAL_ROUNDING
};

class CutoffPriorityBalancer : public Balancer {

public:
    CutoffPriorityBalancer(MPI_Comm& comm, Parameters& params, Statistics& stats) : Balancer(comm, params, stats), localJobs(NULL) {
        
    }
    std::map<int, int> balance(std::map<int, Job*>& jobs) override;

    bool beginBalancing(std::map<int, Job*>& jobs) override;
    bool canContinueBalancing() override;
    bool continueBalancing() override;
    bool handleMessage(MessageHandlePtr handle) override;
    std::map<int, int> getBalancingResult() override;

private:
    std::set<int, PriorityComparator>* localJobs;
    BalancingStage stage;
    ResourcesInfo resourcesInfo;
    int totalVolume;
    std::map<int, float> assignments;
    
};

#endif