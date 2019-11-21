
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
    int totalVolume = (int) (MyMpi::size(_comm) * _load_factor);
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
    std::set<int> excludedNodes = resourcesInfo.allReduce(_comm);
    _stats.increment("reductions"); _stats.increment("broadcasts");
    // "resourcesInfo" now contains global data from all concerned jobs
    if (excludedNodes.count(MyMpi::rank(_comm))) {
        Console::log(Console::VERB, "Ended all-reduction phase. Balancing phase finished.");
        return std::map<int,int>();
    } else {
        Console::log(Console::VERB, "Ended all-reduction phase. Calculating final job demands ...");
    }

    // Assign correct (final) floating-point resources
    float remainingResources = totalVolume - resourcesInfo.assignedResources;
    if (remainingResources < 0.1) remainingResources = 0;

    if (MyMpi::rank(_comm) == 0)
        Console::log(Console::VERB, "Remaining resources: %.3f", remainingResources);

    for (auto it = localJobs.begin(); it != localJobs.end(); ++it) {
        int jobId = *it;
        float demand = getDemand(*jobs[jobId]);
        float priority = jobs[jobId]->getDescription().getPriority();
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
    _assignments.clear();
    _priorities.clear();
    _demands.clear();
    _resources_info = ResourcesInfo();
    _stage = INITIAL_DEMAND;
    _balancing = true;

    // Identify jobs to balance
    _jobs_being_balanced = std::map<int, Job*>();
    assert(_local_jobs == NULL || Console::fail("Found localJobs instance of size %i", _local_jobs->size()));
    _local_jobs = new std::set<int, PriorityComparator>(PriorityComparator(jobs));
    for (auto it : jobs) {
        // Node must be root node to participate
        bool participates = it.second->isRoot();
        // Job must be active, or must be initializing and already having the description
        participates &= it.second->isInState({JobState::ACTIVE, JobState::STANDBY})
                        || (it.second->isInState({JobState::INITIALIZING_TO_ACTIVE}) 
                            && it.second->hasJobDescription());
        if (participates) {
            _jobs_being_balanced[it.first] = it.second;
            _local_jobs->insert(it.first);
        }
    }

    // Find global aggregation of demands
    float aggregatedDemand = 0;
    for (auto it : *_local_jobs) {
        int jobId = it;
        _demands[jobId] = getDemand(*_jobs_being_balanced[jobId]);
        _priorities[jobId] = _jobs_being_balanced[jobId]->getDescription().getPriority();
        aggregatedDemand += _demands[jobId] * _priorities[jobId];
        Console::log(Console::VERB, "Job #%i : demand %i", jobId, _demands[jobId]);
    }
    Console::log(Console::VERB, "Local aggregated demand: %.3f", aggregatedDemand);
    iAllReduce(aggregatedDemand);

    return false; // not finished yet: wait for end of iallreduce
}

bool CutoffPriorityBalancer::canContinueBalancing() {
    if (_stage == INITIAL_DEMAND) {
        // Check if reduction is done
        int flag = 0;
        MPI_Status status;
        MPI_Test(&_reduce_request, &flag, &status);
        return flag;
    }
    if (_stage == REDUCE_RESOURCES || _stage == BROADCAST_RESOURCES) {
        return false; // balancing is advanced by an individual message
    }
    if (_stage == REDUCE_REMAINDERS || _stage == BROADCAST_REMAINDERS) {
        return false; // balancing is advanced by an individual message
    }
    if (_stage == GLOBAL_ROUNDING) {
        // Check if reduction is done
        int flag = 0;
        MPI_Status status;
        MPI_Test(&_reduce_request, &flag, &status);
        return flag;
    }
    return false;
}

bool CutoffPriorityBalancer::continueBalancing() {
    if (_stage == INITIAL_DEMAND) {

        // Finish up initial reduction
        float aggregatedDemand = _reduce_result;
        Console::log(Console::VVERB, "Aggregation of demands: %.3f", aggregatedDemand);

        // Calculate local initial assignments
        _total_volume = (int) (MyMpi::size(_comm) * _load_factor);
        for (auto it : *_local_jobs) {
            int jobId = it;
            float initialMetRatio = _total_volume * _priorities[jobId] / aggregatedDemand;
            _assignments[jobId] = std::min(1.0f, initialMetRatio) * _demands[jobId];
            Console::log(Console::VVERB, "Job #%i : initial assignment %.3f", jobId, _assignments[jobId]);
        }

        // Create ResourceInfo instance with local data
        for (auto it : *_local_jobs) {
            int jobId = it;
            _resources_info.assignedResources += _assignments[jobId];
            _resources_info.priorities.push_back(_priorities[jobId]);
            _resources_info.demandedResources.push_back( _demands[jobId] - _assignments[jobId] );
        }

        // Continue
        return continueBalancing(NULL);
    }

    if (_stage == GLOBAL_ROUNDING) {
        return continueRoundingFromReduction();
    }
    return false;
}

bool CutoffPriorityBalancer::continueBalancing(MessageHandlePtr handle) {
    bool done;
    BalancingStage stage = _stage;
    if (_stage == INITIAL_DEMAND) {
        _stage = REDUCE_RESOURCES;
    }
    if (_stage == REDUCE_RESOURCES) {
        if (handle == NULL || stage != REDUCE_RESOURCES) {
            done = _resources_info.startReduction(_comm);
        } else {
            done = _resources_info.advanceReduction(handle);
        }
        if (done) _stage = BROADCAST_RESOURCES;
    }
    if (_stage == BROADCAST_RESOURCES) {
        if (handle == NULL || stage != BROADCAST_RESOURCES) {
            done = _resources_info.startBroadcast(_comm, _resources_info.getExcludedRanks());
        } else {
            done = _resources_info.advanceBroadcast(handle);
        }
        if (done) {
            if (ITERATIVE_ROUNDING)
                _stage = REDUCE_REMAINDERS;
            else {
                return finishResourcesReduction();
            }
        }
    }
    if (_stage == REDUCE_REMAINDERS) {
        if (handle == NULL || stage != REDUCE_REMAINDERS) {
            finishResourcesReduction();
            done = _remainders.startReduction(_comm);
        } else {
            done = _remainders.advanceReduction(handle);
        }
        if (done) _stage = BROADCAST_REMAINDERS;
    }
    if (_stage == BROADCAST_REMAINDERS) {
        if (handle == NULL || stage != BROADCAST_REMAINDERS) {
            std::set<int> excluded;
            done = _remainders.startBroadcast(_comm, excluded);
        } else {
            done = _remainders.advanceBroadcast(handle);
        }
        if (done) {
            finishRemaindersReduction();
            _stage = GLOBAL_ROUNDING;
        }
    }
    return false;
}

bool CutoffPriorityBalancer::finishResourcesReduction() {

    _stats.increment("reductions"); _stats.increment("broadcasts");

    // "resourcesInfo" now contains global data from all concerned jobs
    if (_resources_info.getExcludedRanks().count(MyMpi::rank(_comm))) {
        Console::log(Console::VERB, "Ended all-reduction phase. Balancing phase finished.");
        _balancing = false;
        delete _local_jobs;
        _local_jobs = NULL;
        _assignments = std::map<int, float>();
        return true;
    } else {
        Console::log(Console::VERB, "Ended all-reduction phase. Calculating final job demands ...");
    }

    // Assign correct (final) floating-point resources
    Console::log(Console::VVERB, "Initially assigned resources: %.3f", _resources_info.assignedResources);
    float remainingResources = _total_volume - _resources_info.assignedResources;
    if (remainingResources < 0.1) remainingResources = 0;

    if (MyMpi::rank(_comm) == 0)
        Console::log(Console::VERB, "Remaining resources: %.3f", remainingResources);

    for (auto it : _jobs_being_balanced) {
        int jobId = it.first;

        float demand = _demands[jobId];
        float priority = _priorities[jobId];
        std::vector<float>& priorities = _resources_info.priorities;
        std::vector<float>& demandedResources = _resources_info.demandedResources;
        std::vector<float>::iterator itPrio = std::find(priorities.begin(), priorities.end(), priority);
        assert(itPrio != priorities.end() || Console::fail("Priority %.3f not found in histogram!", priority));
        int prioIndex = std::distance(priorities.begin(), itPrio);

        if (_assignments[jobId] == demand
            || priorities[prioIndex] <= remainingResources) {
            // Case 1: Assign full demand
            _assignments[jobId] = demand;
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
                _assignments[jobId] += ratio * (demand - _assignments[jobId]);
            }
        }
    }

    if (ITERATIVE_ROUNDING)  {
        // Build object of remainders to be all-reduced
        _remainders = SortedDoubleSequence();
        for (auto it : _jobs_being_balanced) {
            int jobId = it.first;
            _remainders.add(_assignments[jobId]);
        }

        return continueBalancing(NULL);

    } else return true;    
}

bool CutoffPriorityBalancer::finishRemaindersReduction() {
    std::cout << "GLOBAL: ";
    for (int i = 0; i < _remainders.size(); i++) std::cout << _remainders[i] << " ";
    std::cout << std::endl;
    return continueRoundingUntilReduction(0, _remainders.size()-1);
}

bool CutoffPriorityBalancer::continueRoundingUntilReduction(int lower, int upper) {

    _lower_remainder_idx = lower;
    _upper_remainder_idx = upper;

    int idx = (_lower_remainder_idx+_upper_remainder_idx)/2;
    double remainder = _remainders[idx];
    
    _rounded_assignments.clear();
    int localSum = 0;
    for (auto it : _jobs_being_balanced) {
        int jobId = it.first;
        double r = _assignments[jobId] - (int)_assignments[jobId];
        if (r < remainder) _rounded_assignments[jobId] = std::floor(_assignments[jobId]);
        if (r >= remainder) _rounded_assignments[jobId] = std::ceil(_assignments[jobId]);
        localSum += _rounded_assignments[jobId];
    }

    if (_lower_remainder_idx == _upper_remainder_idx) {
        // Finished!
        return true;
    }

    iAllReduce(localSum);
    return false;
}

bool CutoffPriorityBalancer::continueRoundingFromReduction() {

    float utilization = _reduce_result;
    if (std::abs(utilization - _load_factor*MyMpi::size(_comm)) <= 1) {
        // Finished!
        return true;
    }

    int idx = (_lower_remainder_idx+_upper_remainder_idx)/2;
    if (utilization < _load_factor*MyMpi::size(_comm)) {
        // Too few resources utilized
        _upper_remainder_idx = idx-1;
    }
    if (utilization > _load_factor*MyMpi::size(_comm)) {
        // Too many resources utilized
        _lower_remainder_idx = idx+1;
    }
    
    return continueRoundingUntilReduction(_lower_remainder_idx, _upper_remainder_idx);
}

std::map<int, int> CutoffPriorityBalancer::getBalancingResult() {

    // Convert float assignments into actual integer volumes, store and return them
    std::map<int, int> volumes;
    for (auto it = _assignments.begin(); it != _assignments.end(); ++it) {
        int jobId = it->first;
        float assignment = std::max(1.0f, it->second);
        int intAssignment = Random::roundProbabilistically(assignment);
        volumes[jobId] = intAssignment;
        Console::log(Console::VERB, "Job #%i: final assignment %.3f => adjusted to %i", jobId, _assignments[jobId], intAssignment);
    }
    for (auto it = volumes.begin(); it != volumes.end(); ++it) {
        updateVolume(it->first, it->second);
    }

    _balancing = false;
    delete _local_jobs;
    _local_jobs = NULL;
    return volumes;
}
