
#include "event_driven_balancer.h"
#include "util/random.h"
#include "balancing/rounding.h"

bool EventDrivenBalancer::handle(const MessageHandlePtr& handle) {
    if (handle->tag != MSG_ANYTIME_BROADCAST && handle->tag != MSG_ANYTIME_REDUCTION)
        return false;

    int sender = handle->source;
    int myRank = MyMpi::rank(MPI_COMM_WORLD);
    EventMap data; data.deserialize(*(handle->recvData));
    bool done = false;

    //Console::log(Console::VERB, "BLC MSG");
    if (handle->tag == MSG_ANYTIME_REDUCTION) {

        bool reversedTree = sender < myRank;

        // Apply reduction
        data.filterBy(_states);
        _diffs.updateBy(data);
        
        // Forward reduction, switch to broadcast as necessary
        done = reduceIfApplicable(reversedTree ? REVERSED_TREE : NORMAL_TREE);
    }
    if (handle->tag == MSG_ANYTIME_BROADCAST) {

        bool reversedTree = sender > myRank;

        // Forward broadcast
        broadcast(data, reversedTree);

        // Digest data for balancing
        done = digest(data);
    }

    return done;
}

bool EventDrivenBalancer::reduceIfApplicable(int which) {
    // Have anything to reduce?
    if (_diffs.isEmpty()) return false;
    // Enough time passed since last balancing?
    if (Timer::elapsedSeconds() - _last_balancing < _params.getFloatParam("p")) return false;

    // Send to according parents.
    bool done = false;
    if (which == NORMAL_TREE   || which == BOTH) done |= reduce(_diffs, false);
    if (which == REVERSED_TREE || which == BOTH) done |= reduce(_diffs, true);

    // Restart time until a new balancing can be done
    _last_balancing = Timer::elapsedSeconds();

    return done;
}

bool EventDrivenBalancer::reduce(const EventMap& data, bool reversedTree) {
    bool done = false;

    int parent = getParentRank(reversedTree);
    if (parent == MyMpi::rank(MPI_COMM_WORLD)) {

        // No parent / I AM ROOT. 
        
        // Send to other root
        MyMpi::isend(MPI_COMM_WORLD, getRootRank(!reversedTree), MSG_ANYTIME_REDUCTION, data);
        Console::log_send(Console::VERB, getRootRank(!reversedTree), "ROOT_HANDSHAKE");
        
        // Broadcast and digest
        broadcast(data, reversedTree);
        done = digest(data);

    } else {

        // Send to actual parent
        MyMpi::isend(MPI_COMM_WORLD, parent, MSG_ANYTIME_REDUCTION, data);
        //Console::log_send(Console::VERB, parent, "RED");
    }

    return done;     
}

void EventDrivenBalancer::broadcast(const EventMap& data, bool reversedTree) {

    for (int child : getChildRanks(reversedTree)) {
        // Send to actual child
        MyMpi::isend(MPI_COMM_WORLD, child, MSG_ANYTIME_BROADCAST, data);
        Console::log_send(Console::VERB, child, "BRC");
    }
}

bool EventDrivenBalancer::digest(const EventMap& data) {

    bool anyChange = _states.updateBy(data);

    // Filter local diffs by the new "global" state.
    _diffs.filterBy(_states);
    
    if (anyChange) {
        
        // Remove old jobs which have a demand of zero
        _states.removeOldZeros();

        // Calculate and publish new assignments.
        calculateBalancingResult();

        // Begin new reduction, if necessary and enough time passed.
        reduceIfApplicable(BOTH);
    }
    return anyChange;
}

int EventDrivenBalancer::getRootRank(bool reversedTree) {
    if (reversedTree) return MyMpi::size(_comm)-1;
    return 0;
}
int EventDrivenBalancer::getParentRank(bool reversedTree) {
    int myRank = MyMpi::rank(MPI_COMM_WORLD);
    if (reversedTree) myRank = MyMpi::size(_comm)-1 - myRank;
    
    int parent;
    int exp = 2;
    if (myRank == 0) parent = 0;
    else while (true) {
        if (myRank % exp == exp/2) {
            parent = myRank - exp/2;
            break;
        }
        exp *= 2;
    }

    if (reversedTree) parent = MyMpi::size(_comm)-1 - parent;
    return parent;
}
std::vector<int> EventDrivenBalancer::getChildRanks(bool reversedTree) {
    int myRank = MyMpi::rank(MPI_COMM_WORLD);
    if (reversedTree) myRank = MyMpi::size(_comm)-1 - myRank;
    
    std::vector<int> children;
    int exp = MyMpi::size(_comm);
    while (true) {
        if (myRank % exp == 0) {
            int child = myRank + exp/2;
            if (reversedTree) child = MyMpi::size(_comm)-1 - child;
            children.push_back(child);
        }
        exp /= 2;
        if (exp == 1) break;
    }

    return children;
}
bool EventDrivenBalancer::isRoot(int rank, bool reversedTree) {
    return rank == getRootRank(reversedTree);
}
bool EventDrivenBalancer::isLeaf(int rank, bool reversedTree) {
    return rank % 2 == (reversedTree ? 0 : 1);
}

void EventDrivenBalancer::calculateBalancingResult() {

    // 1. Calculate aggregated demand of all jobs
    float aggregatedDemand = 0;
    int numJobs = 0;
    for (const auto& entry : _states.getEntries()) {
        const Event& ev = entry.second; 
        _demands[ev.jobId] = ev.demand;
        _priorities[ev.jobId] = ev.priority;

        if (ev.demand == 0) continue;
        numJobs++;
        aggregatedDemand += (ev.demand-1) * ev.priority;
        Console::log(Console::VERB, "BLC e=%i #%i demand=%i", _balancing_epoch, ev.jobId, ev.demand);
    }
    float totalAvailVolume = MyMpi::size(_comm) * _load_factor - numJobs;

    // 2. Calculate initial assignments and remaining demanded resources
    std::map<int, double> assignments;
    float assignedResources = 0;
    std::map<float, float, std::less<float>> demandedResources;
    for (const auto& entry : _states.getEntries()) {
        const Event& ev = entry.second;
        if (ev.demand == 0) continue;

        double initialMetRatio = totalAvailVolume * ev.priority / aggregatedDemand;
        // job demand minus "atomic" demand that is met by default
        int remainingDemand = ev.demand - 1;
        // assignment: atomic node plus fair share of reduced aggregation
        assignments[ev.jobId] = 1 + std::min(1.0, initialMetRatio) * remainingDemand;
        assignedResources += assignments[ev.jobId] - 1;
        if (!demandedResources.count(ev.priority)) demandedResources[ev.priority] = 0;
        demandedResources[ev.priority] += ev.demand - assignments[ev.jobId];
        Console::log(Console::VVERB, "BLC e=%i #%i init_assignment=%.3f", _balancing_epoch, ev.jobId, assignments[ev.jobId]);
    }

    // 3. Calculate final floating-point assignments for all jobs

    int rank = MyMpi::rank(MPI_COMM_WORLD);
    Console::log(!rank ? Console::VVERB : Console::VVVVERB, "BLC e=%i init_assigned_resources=%.3f", 
        _balancing_epoch, assignedResources);
    
    // Atomic job assignments are already subtracted from _total_avail_volume
    // and are not part of the all-reduced assignedResources either
    float remainingResources = totalAvailVolume - assignedResources;
    if (remainingResources < 0.1) remainingResources = 0; // too low a remainder to make a difference
    Console::log(!rank ? Console::VVERB : Console::VVVVERB, "BLC e=%i remaining_resources=%.3f", _balancing_epoch, remainingResources);

    std::map<int, int> allVolumes;
    for (const auto& entry : _states.getEntries()) {
        const Event& ev = entry.second;
        if (ev.demand <= 1) continue;

        int jobId = ev.jobId;
        int demand = _demands[jobId];
        float priority = _priorities[jobId];
        float prevPriority = -1;
        for (const auto& entry : demandedResources) {
            if (entry.first == priority) break;
            prevPriority = entry.first;
        }

        if (assignments[jobId] >= demand
            || demandedResources[priority] <= remainingResources) {
            // Case 1: Assign full demand
            assignments[jobId] = demand;
        } else {
            if (prevPriority == -1 || demandedResources[prevPriority] >= remainingResources) {
                // Case 2: No additional resources assigned
            } else {
                // Case 3: Evenly distribute ratio of remaining resources
                assert(remainingResources >= 0);
                double ratio = (remainingResources - demandedResources[prevPriority])
                            / (demandedResources[priority] - demandedResources[prevPriority]);
                assert(ratio > 0);
                assert(ratio <= 1);
                assignments[jobId] += ratio * (demand - assignments[jobId]);
            }
        }

        Console::log(Console::VVERB, "BLC e=%i #%i adj_assignment=%.3f", _balancing_epoch, jobId, assignments[jobId]);
    }

    // 4. Round job assignments

    if (_params.getParam("r") == ROUNDING_FLOOR) {
        // Round by flooring
        for (const auto& entry : _states.getEntries()) {
            allVolumes[entry.first] = std::floor(assignments[entry.first]);
        }
    } else if (_params.getParam("r") == ROUNDING_PROBABILISTIC) {
        // Round probabilistically
        for (const auto& entry : _states.getEntries()) {
            allVolumes[entry.first] = Random::roundProbabilistically(assignments[entry.first]);
        }
    } else if (_params.getParam("r") == ROUNDING_BISECTION) {

        // Calculate optimal rounding by bisection

        SortedDoubleSequence remainders;
        for (const auto& entry : _states.getEntries()) {
            double remainder = assignments[entry.first] - (int)assignments[entry.first];
            if (remainder > 0 && remainder < 1) remainders.add(remainder);
        }
        int lower = 0, upper = remainders.size();
        int idx = (lower+upper)/2;
        int iterations = 0;
        float lastUtilization;

        int bestRemainderIdx = -1;
        float bestPenalty;
        float bestUtilization;

        while (true) {
            int utilization = 0;
            if (idx <= remainders.size()) {
                // Remainder is either one of the remainders from the reduced sequence
                // or the right-hand limit 1.0
                double remainder = (idx < remainders.size() ? remainders[idx] : 1.0);
                // Round your local assignments and calculate utilization sum
                _volumes = Rounding::getRoundedAssignments(idx, utilization, remainders, assignments);
            }

            // Store result, if it is the best one so far
            float p = Rounding::penalty((float)utilization / MyMpi::size(_comm), _load_factor);
            if (bestRemainderIdx == -1 || p < bestPenalty) {
                bestPenalty = p;
                bestRemainderIdx = idx;
                bestUtilization = utilization;
            }

            // Log iteration
            if (!remainders.isEmpty() && idx <= remainders.size()) {
                double remainder = (idx < remainders.size() ? remainders[idx] : 1.0);
                Console::log(Console::VVERB, "BLC e=%i ROUNDING it=%i [%i,%i]=>%i rmd=%.3f util=%.2f pen=%.2f", 
                                _balancing_epoch, iterations, lower, upper, idx,
                                remainder, (float)utilization, p);
            }

            // Termination?
            if (utilization == lastUtilization) { // Utilization unchanged?
                // Finished!
                if (!remainders.isEmpty() && bestRemainderIdx <= remainders.size()) {
                    // Remainders are known to this node: apply and report
                    int sum = 0;
                    allVolumes = Rounding::getRoundedAssignments(bestRemainderIdx, sum, remainders, assignments);

                    double remainder = (bestRemainderIdx < remainders.size() ? remainders[bestRemainderIdx] : 1.0);
                    Console::log(Console::VVERB, 
                                "BLC e=%i ROUNDING_DONE its=%i rmd=%.3f util=%.2f pen=%.2f", 
                                _balancing_epoch, iterations, remainder, bestUtilization, bestPenalty);
                }
                break;

            } else if (lower < upper) {
                if (utilization < _load_factor*MyMpi::size(_comm)) {
                    // Too few resources utilized
                    upper = idx-1;
                }
                if (utilization > _load_factor*MyMpi::size(_comm)) {
                    // Too many resources utilized
                    lower = idx+1;
                }
            }
            
            lastUtilization = utilization;
        }
    }

    // 5. Only remember job assignments that are of a local job
    _volumes.clear();
    for (const auto& pair : _jobs_being_balanced) {
        if (allVolumes[pair.first] >= 1)
            _volumes[pair.first] = allVolumes[pair.first];
    }
}