
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








bool CutoffPriorityBalancer::beginBalancing(std::map<int, Job*>& jobs) {
    
    // Initialize
    assignments.clear();
    priorities.clear();
    demands.clear();
    resourcesInfo = ResourcesInfo();
    stage = INITIAL_DEMAND;
    balancing = true;

    // Identify jobs to balance
    jobsBeingBalanced = std::map<int, Job*>();
    assert(localJobs == NULL || Console::fail("Found localJobs instance of size %i", localJobs->size()));
    localJobs = new std::set<int, PriorityComparator>(PriorityComparator(jobs));
    for (auto it : jobs) {
        if (it.second->isInState({JobState::ACTIVE, JobState::INITIALIZING_TO_ACTIVE}) && it.second->isRoot()) {
            jobsBeingBalanced[it.first] = it.second;
            JobDescription& desc = it.second->getDescription();
            localJobs->insert(desc.getId());
        }
    }

    // Find global aggregation of demands
    float aggregatedDemand = 0;
    for (auto it : *localJobs) {
        int jobId = it;
        demands[jobId] = getDemand(*jobsBeingBalanced[jobId]);
        priorities[jobId] = jobsBeingBalanced[jobId]->getDescription().getPriority();
        aggregatedDemand += demands[jobId] * priorities[jobId];
        Console::log(Console::VERB, "Job #%i : demand %i", jobId, demands[jobId]);
    }
    Console::log(Console::VERB, "Local aggregated demand: %.3f", aggregatedDemand);
    iAllReduce(aggregatedDemand);

    return false; // not finished yet
}

bool CutoffPriorityBalancer::canContinueBalancing() {
    if (stage == INITIAL_DEMAND) {
        // Check if reduction is done
        int flag = 0;
        MPI_Status status;
        MPI_Test(&reduceRequest, &flag, &status);
        return flag;
    }
    if (stage == REDUCE_RESOURCES || stage == BROADCAST_RESOURCES) {
        return false; // balancing is terminated by an individual message
    }
    return false;
}

bool CutoffPriorityBalancer::continueBalancing() {
    if (stage == INITIAL_DEMAND) {

        // Finish up initial reduction
        float aggregatedDemand = reduceResult;
        Console::log(Console::VVERB, "Aggregation of demands: %.3f", aggregatedDemand);

        stage = REDUCE_RESOURCES;

        // Calculate local initial assignments
        totalVolume = (int) (MyMpi::size(comm) * loadFactor);
        for (auto it : *localJobs) {
            int jobId = it;
            float initialMetRatio = totalVolume * priorities[jobId] / aggregatedDemand;
            assignments[jobId] = std::min(1.0f, initialMetRatio) * demands[jobId];
            Console::log(Console::VVERB, "Job #%i : initial assignment %.3f", jobId, assignments[jobId]);
        }

        // Create ResourceInfo instance with local data
        for (auto it : *localJobs) {
            int jobId = it;
            resourcesInfo.assignedResources += assignments[jobId];
            resourcesInfo.priorities.push_back(priorities[jobId]);
            resourcesInfo.demandedResources.push_back( demands[jobId] - assignments[jobId] );
        }
        // AllReduce
        bool done = resourcesInfo.startReduction(comm);
        if (done) {
            stage = BROADCAST_RESOURCES;
            done = resourcesInfo.startBroadcast(comm, resourcesInfo.getExcludedRanks());
            if (done) {
                return true;
            }
        }
        return false;
    }
    return false;
}

bool CutoffPriorityBalancer::handleMessage(MessageHandlePtr handle) {
    bool done;
    if (stage == REDUCE_RESOURCES) {
        done = resourcesInfo.advanceReduction(handle);
        if (done) {
            stage = BROADCAST_RESOURCES;
            done = resourcesInfo.startBroadcast(comm, resourcesInfo.getExcludedRanks());
            if (done) {
                return true;
            }
        }
    } else if (stage == BROADCAST_RESOURCES) {
        done = resourcesInfo.advanceBroadcast(handle);
        if (done) {
            return true;
        }
    }
    return false;
}

std::map<int, int> CutoffPriorityBalancer::getBalancingResult() {

    stats.increment("reductions"); stats.increment("broadcasts");

    // "resourcesInfo" now contains global data from all concerned jobs
    if (resourcesInfo.getExcludedRanks().count(MyMpi::rank(comm))) {
        Console::log(Console::VERB, "Ended all-reduction phase. Balancing phase finished.");
        balancing = false;
        delete localJobs;
        localJobs = NULL;
        return std::map<int, int>();
    } else {
        Console::log(Console::VERB, "Ended all-reduction phase. Calculating final job demands ...");
    }

    // Assign correct (final) floating-point resources
    Console::log(Console::VVERB, "Initially assigned resources: %.3f", resourcesInfo.assignedResources);
    float remainingResources = totalVolume - resourcesInfo.assignedResources;
    if (remainingResources < 0.1) remainingResources = 0;

    if (MyMpi::rank(comm) == 0)
        Console::log(Console::VERB, "Remaining resources: %.3f", remainingResources);
    
    for (auto it : jobsBeingBalanced) {
        int jobId = it.first;

        float demand = demands[jobId];
        float priority = 0.001f * ((int) (1000 * priorities[jobId]));
        std::vector<float>& priorities = resourcesInfo.priorities;
        std::vector<float>& demandedResources = resourcesInfo.demandedResources;
        std::vector<float>::iterator itPrio = std::find(priorities.begin(), priorities.end(), priority);
        assert(itPrio != priorities.end() || Console::fail("Priority %.3f not found in histogram!", priority));
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

    balancing = false;
    delete localJobs;
    localJobs = NULL;
    return volumes;
}