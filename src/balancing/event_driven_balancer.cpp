
#include "event_driven_balancer.h"

bool EventDrivenBalancer::handle(const MessageHandlePtr& handle) {
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

    int child = getChildRank(reversedTree);
    if (child != MyMpi::rank(MPI_COMM_WORLD)) {
        // Send to actual child
        MyMpi::isend(MPI_COMM_WORLD, getChildRank(reversedTree), MSG_ANYTIME_BROADCAST, data);
        Console::log_send(Console::VERB, getChildRank(reversedTree), "BRC");
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
int EventDrivenBalancer::getChildRank(bool reversedTree) {
    int myRank = MyMpi::rank(MPI_COMM_WORLD);
    if (reversedTree) myRank = MyMpi::size(_comm)-1 - myRank;
    
    int child;
    int exp = MyMpi::size(_comm);
    if (myRank == 0) child = 1;
    else while (true) {
        if (myRank % exp == 0) {
            child = myRank + exp/2;
            break;
        }
        exp /= 2;
    }

    if (reversedTree) child = MyMpi::size(_comm)-1 - child;
    return child;
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
        if (ev.demand == 0) continue;

        aggregatedDemand += (ev.demand-1) * ev.priority;
        Console::log(Console::VERB, "BLC e=%i #%i demand=%i", _balancing_epoch, ev.jobId, ev.demand);
    }
    float totalAvailVolume = MyMpi::size(_comm) * _load_factor - numJobs;

    // 2. Calculate initial assignments and remaining demanded resources
    std::map<int, float> assignments;
    float assignedResources = 0;
    std::map<float, float, std::greater<float>> demandedResources;
    for (const auto& entry : _states.getEntries()) {
        const Event& ev = entry.second;
        if (ev.demand == 0) continue;

        double initialMetRatio = totalAvailVolume * ev.priority / aggregatedDemand;
        // job demand minus "atomic" demand that is met by default
        int remainingDemand = ev.demand - 1;
        // assignment: atomic node plus fair share of reduced aggregation
        assignments[ev.jobId] = 1 + std::min(1.0, initialMetRatio) * remainingDemand;
        assignedResources += assignments[ev.jobId];
        demandedResources[ev.priority];
        demandedResources[ev.priority] += ev.demand - assignments[ev.jobId];
        Console::log(Console::VVERB, "BLC e=%i #%i init_assignment=%.3f", _balancing_epoch, ev.jobId, assignments[ev.jobId]);
    }

    // 3. Calculate final assignments for all LOCAL jobs

    int rank = MyMpi::rank(MPI_COMM_WORLD);
    Console::log(!rank ? Console::VVERB : Console::VVVVERB, "BLC e=%i init_assigned_resources=%.3f", 
        _balancing_epoch, assignedResources);
    
    // Atomic job assignments are already subtracted from _total_avail_volume
    // and are not part of the all-reduced assignedResources either
    float remainingResources = totalAvailVolume - assignedResources;
    if (remainingResources < 0.1) remainingResources = 0; // too low a remainder to make a difference
    Console::log(!rank ? Console::VVERB : Console::VVVVERB, "BLC e=%i remaining_resources=%.3f", _balancing_epoch, remainingResources);

    _balancing_result.clear();
    for (auto it : _jobs_being_balanced) {
        int jobId = it.first;
        if (_demands[jobId] == 1) continue;

        int demand = _demands[jobId];
        float priority = _priorities[jobId];
        int prioIndex = 0;
        for (const auto& entry : demandedResources) {
            if (entry.first == priority) break;
            prioIndex++;
        }

        if (assignments[jobId] >= demand
            || demandedResources[priority] <= remainingResources) {
            // Case 1: Assign full demand
            assignments[jobId] = demand;
        } else {
            if (prioIndex == 0 || demandedResources[prioIndex-1] >= remainingResources) {
                // Case 2: No additional resources assigned
            } else {
                // Case 3: Evenly distribute ratio of remaining resources
                assert(remainingResources >= 0);
                double ratio = (remainingResources - demandedResources[prioIndex-1])
                            / (demandedResources[prioIndex] - demandedResources[prioIndex-1]);
                assert(ratio > 0);
                assert(ratio <= 1);
                assignments[jobId] += ratio * (demand - assignments[jobId]);
            }
        }

        // Round by flooring
        _balancing_result[jobId] = std::floor(assignments[jobId]);
        
        Console::log(Console::VVERB, "BLC e=%i #%i adj_assignment=%.3f", _balancing_epoch, jobId, assignments[jobId]);
    }
}