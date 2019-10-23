

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

std::vector<int> serialize() const override {
    std::vector<int> data;
    data.push_back((int) (assignedResources * 1000));
    for (size_t i = 0; i < priorities.size(); i++) {
        data.push_back((int) (priorities[i] * 1000));
    }
    for (size_t i = 0; i < demandedResources.size(); i++) {
        data.push_back((int) (demandedResources[i] * 1000));
    }
    return data;
}
void deserialize(const std::vector<int>& packed) override {
    assignedResources = 0.001f * packed[0];
    priorities.clear();
    demandedResources.clear();
    int size = (packed.size() - 1) / 2;
    for (int i = 0; i < size; i++) {
        priorities.push_back(0.001f * packed[1+i]);
        demandedResources.push_back(0.001f * packed[1+i+size]);
    }
}
std::unique_ptr<Reduceable> getDeserialized(const std::vector<int>& packed) const override {
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