
#include <utility>
#include <assert.h>

#include "cutoff_priority_balancer.h"
#include "util/random.h"
#include "util/console.h"


bool CutoffPriorityBalancer::beginBalancing(std::map<int, Job*>& jobs) {

    // Initialize
    _assignments.clear();
    _priorities.clear();
    _demands.clear();
    _resources_info = ResourcesInfo();
    _stage = INITIAL_DEMAND;
    _balancing = true;

    // Identify jobs to balance
    bool isWorkerBusy = false;
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
        // Set "busy" flag of this worker node to true, if applicable
        if (it.second->isInState({JobState::ACTIVE, JobState::INITIALIZING_TO_ACTIVE})) {
            isWorkerBusy = true;
        }
    }

    // Find global aggregation of demands and amount of busy nodes
    float aggregatedDemand = 0;
    for (auto it : *_local_jobs) {
        int jobId = it;
        _demands[jobId] = getDemand(*_jobs_being_balanced[jobId]);
        _priorities[jobId] = _jobs_being_balanced[jobId]->getDescription().getPriority();
        aggregatedDemand += _demands[jobId] * _priorities[jobId];
        Console::log(Console::VERB, "Job #%i : demand %i", jobId, _demands[jobId]);
    }
    Console::log(Console::VERB, "Local aggregated demand: %.3f", aggregatedDemand);
    _demand_and_busy_nodes_contrib[0] = aggregatedDemand;
    _demand_and_busy_nodes_contrib[1] = (isWorkerBusy ? 1 : 0);
    _demand_and_busy_nodes_result[0] = 0;
    _demand_and_busy_nodes_result[1] = 0;
    _reduce_request = MyMpi::iallreduce(_comm, _demand_and_busy_nodes_contrib, _demand_and_busy_nodes_result, 2);

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
        float aggregatedDemand = _demand_and_busy_nodes_result[0];
        int busyNodes = _demand_and_busy_nodes_result[1];
        bool rankZero = MyMpi::rank(MPI_COMM_WORLD) == 0;
        Console::log(rankZero ? Console::VVERB : Console::VVVVERB, 
            "%i/%i nodes (%.2f%%) are busy", (int)busyNodes, MyMpi::size(_comm), 
            ((float)100*busyNodes)/MyMpi::size(_comm));
        Console::log(rankZero ? Console::VVERB : Console::VVVVERB, 
            "Aggregation of demands: %.3f", aggregatedDemand);

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
        Console::log(Console::VERB, "Ended all-reduction. Balancing finished.");
        _balancing = false;
        delete _local_jobs;
        _local_jobs = NULL;
        _assignments = std::map<int, float>();
        return true;
    } else {
        Console::log(Console::VERB, "Ended all-reduction. Calculating final job demands");
    }

    // Assign correct (final) floating-point resources
    if (MyMpi::rank(MPI_COMM_WORLD) == 0)
        Console::log(Console::VVERB, "Initially assigned resources: %.3f", _resources_info.assignedResources);
    else
        Console::log(Console::VVVVERB, "Initially assigned resources: %.3f", _resources_info.assignedResources);
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
        Console::log(Console::VERB, " #%i : final assignment %.3f => adj. to %i", jobId, _assignments[jobId], intAssignment);
    }
    for (auto it = volumes.begin(); it != volumes.end(); ++it) {
        updateVolume(it->first, it->second);
    }

    _balancing = false;
    delete _local_jobs;
    _local_jobs = NULL;
    return volumes;
}
