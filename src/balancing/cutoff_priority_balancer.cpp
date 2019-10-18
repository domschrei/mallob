
#include <utility>
#include <assert.h>

#include "cutoff_priority_balancer.h"
#include "util/random.h"
#include "util/console.h"

std::map<int, int> CutoffPriorityBalancer::balance(std::map<int, Job*>& jobs) {

    // Identify jobs to balance
    std::set<int, PriorityComparator> localJobs = std::set<int, PriorityComparator>(PriorityComparator(jobs));
    for (auto it = jobs.begin(); it != jobs.end(); ++it) {
        Job &job = *it->second;
        if ((job.getState() == JobState::ACTIVE) && job.isRoot()) {
            //Console::log(Console::VVERB, "Participating with " + img.toStr() + ", ID " + std::to_string(img.getJob()->getId()));
            JobDescription& desc = job.getDescription();
            localJobs.insert(desc.getId());
        }
    }

    // Find global aggregation of demands
    float aggregatedDemand = 0;
    for (auto it = localJobs.begin(); it != localJobs.end(); ++it) {
        int jobId = *it;
        int demand = getDemand(*jobs[jobId]);
        aggregatedDemand += demand * jobs[jobId]->getDescription().getPriority();
        Console::log(Console::VERB, "Job #%i : demand %i", jobId, demand);
    }
    aggregatedDemand = allReduce(aggregatedDemand);

    // Calculate local initial assignments
    int totalVolume = (int) (MyMpi::size(comm) * loadFactor);
    std::map<int, float> assignments;
    for (auto it = localJobs.begin(); it != localJobs.end(); ++it) {
        int jobId = *it;
        float initialMetRatio = totalVolume * jobs[jobId]->getDescription().getPriority() / aggregatedDemand;
        assignments[jobId] = std::min(1.0f, initialMetRatio) * getDemand(*jobs[jobId]);
        Console::log(Console::VERB, "Job #%i : initial assignment", jobId, assignments[jobId]);
    }

    // All-Reduce resources information

    // Create ResourceInfo instance with local data
    ResourcesInfo resourcesInfo;
    for (auto it = localJobs.begin(); it != localJobs.end(); ++it) {
        int jobId = *it;
        resourcesInfo.assignedResources += assignments[jobId];
        resourcesInfo.priorities.push_back(jobs[jobId]->getDescription().getPriority());
        resourcesInfo.demandedResources.push_back( getDemand(*jobs[jobId]) - assignments[jobId] );
    }
    // AllReduce
    std::set<int> excludedNodes = resourcesInfo.allReduce(comm);
    stats.increment("reductions"); stats.increment("broadcasts");
    // "resourcesInfo" now contains global data from all concerned jobs
    if (excludedNodes.count(MyMpi::rank(comm))) {
        Console::log(Console::VERB, "Ended all-reduction phase. Balancing phase finished.");
        return std::map<int, int>();
    } else {
        Console::log(Console::VERB, "Ended all-reduction phase. Calculating final job demands ...");
    }

    // Assign correct (final) floating-point resources
    float remainingResources = totalVolume - resourcesInfo.assignedResources;
    if (remainingResources < 0.1) remainingResources = 0;

    if (MyMpi::rank(comm) == 0)
        Console::log(Console::VERB, "Remaining resources: %.3f", remainingResources);
    
    for (auto it = localJobs.begin(); it != localJobs.end(); ++it) {
        int jobId = *it;
        float demand = getDemand(*jobs[jobId]);
        float priority = 0.001f * ((int) (1000 * jobs[jobId]->getDescription().getPriority()));
        std::vector<float>& priorities = resourcesInfo.priorities;
        std::vector<float>& demandedResources = resourcesInfo.demandedResources;
        std::vector<float>::iterator itPrio = std::find(priorities.begin(), priorities.end(), priority);
        assert(itPrio != priorities.end());
        int prioIndex = std::distance(priorities.begin(), itPrio);

        if (assignments[jobId] == demand 
            || priorities[prioIndex] <= remainingResources) {
            // Case 1: Assign full demand
            assignments[jobId] = demand;
        } else {
            if (prioIndex == 0 || demandedResources[prioIndex-1] >= remainingResources) {
                // Case 2: No additional resources assigned
            } else {
                // Case 3: Evenly distribute ratio of remaining resources
                assert(remainingResources >= 0);
                float ratio = (remainingResources - demandedResources[prioIndex-1]) 
                            / (demandedResources[prioIndex] - demandedResources[prioIndex-1]);
                assert(ratio > 0);
                assert(ratio <= 1);
                assignments[jobId] += ratio * (demand - assignments[jobId]);
            }
        }
    }

    // Convert float assignments into actual integer volumes, store and return them
    std::map<int, int> volumes;
    for (auto it = assignments.begin(); it != assignments.end(); ++it) {
        int jobId = it->first;
        float assignment = std::max(1.0f, it->second);
        int intAssignment = Random::roundProbabilistically(assignment);
        volumes[jobId] = intAssignment;
        Console::log(Console::VERB, "Job #%i: final assignment %.3f => adjusted to %i", jobId, assignments[jobId], intAssignment);
    }
    for (auto it = volumes.begin(); it != volumes.end(); ++it) {
        updateVolume(it->first, it->second);
    }

    //MPI_Barrier(comm);

    return volumes;
}