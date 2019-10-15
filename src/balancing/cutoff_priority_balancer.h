

#ifndef DOMPASCH_BALANCER_CUTOFF_PRIORITY_H
#define DOMPASCH_BALANCER_CUTOFF_PRIORITY_H

#include <utility>

#include "balancing/balancer.h"
#include "data/serializable.h"

class PriorityComparator {
private:
    std::map<int, Job*>& jobs;
public:
    PriorityComparator(std::map<int, Job*>& jobs) : jobs(jobs) {}
    bool operator() (const int& lhs, const int& rhs) const {
        return jobs[lhs]->getDescription().getPriority() > jobs[rhs]->getDescription().getPriority();
    }
};

struct ResourcesInfo : public Serializable {
float assignedResources;
std::vector<float> priorities;
std::vector<float> demandedResources;

ResourcesInfo() : 
    assignedResources(0), 
    priorities(std::vector<float>()), 
    demandedResources(std::vector<float>()) {}

void merge(const ResourcesInfo& other) {
    assignedResources += other.assignedResources;
    for (int i = 0; i < other.priorities.size(); i++) {

        float prioToInsert = other.priorities[i];

        // Find insertion point
        int idx = 0;
        while (idx < priorities.size() && priorities[idx] > prioToInsert) idx++;

        // If this priority did not exist before, 
        // insert an element into both structures
        if (idx >= priorities.size() || priorities[idx] != prioToInsert) {
            priorities.insert(priorities.begin()+idx, prioToInsert);
            demandedResources.insert(demandedResources.begin()+idx, 0);
        }
        // Update demanded resources
        demandedResources[idx] += other.demandedResources[i];
    }
}

std::vector<int> serialize() const override {
    std::vector<int> data;
    data.push_back((int) (assignedResources * 1000));
    for (int i = 0; i < priorities.size(); i++) {
        data.push_back((int) (priorities[i] * 1000));
    }
    for (int i = 0; i < demandedResources.size(); i++) {
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
};

class CutoffPriorityBalancer : public Balancer {

public:
    CutoffPriorityBalancer(MPI_Comm& comm, Parameters params) : Balancer(comm, params) {
        
    }
    std::map<int, int> balance(std::map<int, Job*>& jobs) override;

};

#endif