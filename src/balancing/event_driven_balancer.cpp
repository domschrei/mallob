
#include <climits>

#include "event_driven_balancer.hpp"
#include "util/random.hpp"
#include "balancing/rounding.hpp"

EventDrivenBalancer::EventDrivenBalancer(MPI_Comm& comm, Parameters& params) : Balancer(comm, params) {
    _last_balancing = 0;

    log(V4_VVER, "BLC_TREE_NORMAL parent: %i\n", getParentRank(false));
    log(V4_VVER, "BLC_TREE_NORMAL children: ");
    for (int child : getChildRanks(false)) log(LOG_NO_PREFIX | V4_VVER, "%i ", child);
    log(LOG_NO_PREFIX | V4_VVER, ".\n");

    log(V4_VVER, "BLC_TREE_REVERSED parent: %i\n", getParentRank(true));
    log(V4_VVER, "BLC_TREE_REVERSED children: ");
    for (int child : getChildRanks(true)) log(LOG_NO_PREFIX | V4_VVER, "%i ", child);
    log(LOG_NO_PREFIX | V4_VVER, ".\n");
}

bool EventDrivenBalancer::beginBalancing(robin_hood::unordered_map<int, Job*>& jobs) {

    // Identify jobs to balance
    _jobs_being_balanced = robin_hood::unordered_map<int, Job*>();
    for (const auto& [id, job] : jobs) if (job->getJobTree().isRoot()) {
        
        if (_job_epochs.count(id) && job->getState() == PAST) {
            
            // Job might have been active just before: Signal its termination
            Event ev({id, /*epoch=*/INT_MAX, /*demand=*/0, /*priority=*/0});
            if (_states.getEntries().count(id) || _diffs.getEntries().count(id)) {
                // Job is registered, possibly with non-zero demand: try to insert into diffs map
                bool inserted = _diffs.insertIfNovel(ev);
                if (inserted) {
                    log(V3_VERB, "(1) JOBEVENT #%i d=%i p=%.2f e=%i\n", ev.jobId, ev.demand, ev.priority, ev.epoch);
                    _job_epochs.erase(id);
                }
            }
        
        } else if (job->getState() != PAST) {

            // Job participates
            _jobs_being_balanced[id] = job;
            
            if (!_job_epochs.count(id)) {
                // Completely new!
                _job_epochs[id] = 1;
            }

            // Insert this job as an event, if there is something novel about it
            Event ev;
            if (job->getState() == STANDBY) {
                // Job is in STANDBY mode: set demand of zero
                ev = Event({id, /*epoch=*/_job_epochs[id], /*demand=*/0, /*priority=*/job->getPriority()});
            } else {
                // Job must be active
                int epoch = _job_epochs[id];
                int demand = std::max(1, getDemand(*job));
                ev = Event({id, epoch, demand, job->getPriority()});
            }
            if (!_states.getEntries().count(id) 
                    || ev.demand != _states.getEntries().at(id).demand 
                    || ev.priority != _states.getEntries().at(id).priority) {
                // Not contained yet in state: try to insert into diffs map
                bool inserted = _diffs.insertIfNovel(ev);
                if (inserted) {
                    log(V3_VERB, "(2) JOBEVENT #%i d=%i p=%.2f e=%i\n", ev.jobId, ev.demand, ev.priority, _job_epochs[id]);
                    _job_epochs[id]++;
                } 
            }
        }
    }

    // initiate a balancing, if applicable
    return reduceIfApplicable(BOTH);
}

bool EventDrivenBalancer::handle(MessageHandle& handle) {
    if (handle.tag != MSG_BROADCAST_DATA && handle.tag != MSG_REDUCE_DATA)
        return false;
    
    log(V5_DEBG, "BLC: handle\n");

    int sender = handle.source;
    int myRank = MyMpi::rank(MPI_COMM_WORLD);
    EventMap data = Serializable::get<EventMap>(handle.getRecvData());
    bool done = false;

    //log(V3_VERB, "BLC MSG\n");
    if (handle.tag == MSG_REDUCE_DATA) {

        bool reversedTree = sender < myRank;

        // Apply reduction
        //data.filterBy(_states);
        _diffs.updateBy(data);
        
        // Forward reduction, switch to broadcast as necessary
        done = reduceIfApplicable(reversedTree ? REVERSED_TREE : NORMAL_TREE);
    }
    if (handle.tag == MSG_BROADCAST_DATA) {

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
    if (Timer::elapsedSeconds() - _last_balancing < _params.balancingPeriod()) return false;

    if (which == BOTH) log(V4_VVER, "Initiate balancing (%i diffs)\n", _diffs.getEntries().size());

    // Send to according parents.
    bool done = false;
    if (which == NORMAL_TREE   || which == BOTH) done |= reduce(_diffs, false);
    if (which == REVERSED_TREE || which == BOTH) done |= reduce(_diffs, true);

    // Restart time until a new balancing can be done
    _last_balancing = Timer::elapsedSeconds();

    return done;
}

bool EventDrivenBalancer::reduce(EventMap& data, bool reversedTree) {
    bool done = false;

    if (MyMpi::size(_comm) == 1) {
        // Only a single node -- locally digest
        return digest(data);
    }

    int parent = getParentRank(reversedTree);
    if (parent == MyMpi::rank(MPI_COMM_WORLD)) {
        // No parent / I AM ROOT. 
        
        // Send to other root
        MyMpi::isend(getRootRank(!reversedTree), MSG_BROADCAST_DATA, data);
        log(LOG_ADD_DESTRANK | V5_DEBG, "BLC root handshake", getRootRank(!reversedTree));

        // Broadcast and digest
        broadcast(data, reversedTree);
        done = digest(data);

    } else {

        // Send to actual parent
        MyMpi::isend(parent, MSG_REDUCE_DATA, data);
        //log(LOG_ADD_DESTRANK | V3_VERB, parent, "RED");
    }

    return done;     
}

void EventDrivenBalancer::broadcast(EventMap& data, bool reversedTree) {

    // List of recently broadcast event maps
    std::list<EventMap>& recentBroadcasts = (reversedTree ? _recent_broadcasts_reversed : _recent_broadcasts_normal);

    // Do not send an empty event map
    if (data.isEmpty()) return;

    // Check that the current event map was not recently sent
    bool doSend = true;
    for (const EventMap& recentMap : recentBroadcasts) {
        if (recentMap == data) {
            doSend = false;
            break;
        }
    }
    if (!doSend) return;

    if (isRoot(MyMpi::rank(MPI_COMM_WORLD), reversedTree)) {
        // Root
        data.bumpGlobalEpoch();
    }

    // Add current event map to currently broadcast maps
    recentBroadcasts.push_front(data);
    if (recentBroadcasts.size() > RECENT_BROADCAST_MEMORY) 
        recentBroadcasts.resize(RECENT_BROADCAST_MEMORY);

    // Do broadcast
    for (int child : getChildRanks(reversedTree)) {
        // Send to actual child
        MyMpi::isend(child, MSG_BROADCAST_DATA, data);
        //log(LOG_ADD_DESTRANK | V3_VERB, child, "BRC");
    }
}

bool EventDrivenBalancer::digest(const EventMap& data) {
    log(V4_VVER, "BLC DIGEST epoch=%ld\n", data.getGlobalEpoch());

    bool anyChange = _states.updateBy(data);

    // Filter local diffs by the new "global" state.
    size_t diffSize = _diffs.getEntries().size();
    _diffs.filterBy(_states);
    log(V3_VERB, "BLC digest %i diffs, %i/%i local diffs remaining\n", 
            data.getEntries().size(), _diffs.getEntries().size(), diffSize);
    
    if (anyChange) {
        // Successful balancing: Bump epoch
        _balancing_epoch++;
    }
    return anyChange;
}

int EventDrivenBalancer::getRootRank(bool reversedTree) {
    if (reversedTree) {
        int size = MyMpi::size(_comm);
        if (size % 2 == 1) return size-2;
        else return size-1;
    }
    return 0;
}
int EventDrivenBalancer::getParentRank(bool reversedTree) {

    int size = MyMpi::size(_comm);
    int myRank = MyMpi::rank(MPI_COMM_WORLD);

    // Offset tree by one when total number of nodes is odd
    if (reversedTree && size % 2 == 1) {
        size--;
        // rightmost node
        if (myRank == size) return size-1;
    }
    // Reverse tree
    if (reversedTree) myRank = size-1 - myRank;
    
    int parent;
    int exp = 2;
    if (myRank == 0) parent = 0;
    else while (true) {
        if (myRank % exp == exp/2 
                && myRank - exp/2 >= 0) {
            parent = myRank - exp/2;
            break;
        }
        exp *= 2;
    }

    // Un-reverse tree
    if (reversedTree) parent = size-1 - parent;
    return parent;
}
std::vector<int> EventDrivenBalancer::getChildRanks(bool reversedTree) {

    int size = MyMpi::size(_comm);
    int myRank = MyMpi::rank(MPI_COMM_WORLD);
    std::vector<int> children;

    if (size == 1) return children;

    // Offset tree by one when total number of nodes is odd
    if (reversedTree && size % 2 == 1) {
        size--;
        // rightmost node. No children
        if (myRank == size) return std::vector<int>();
        // left to rightmost node: root. One additional child
        if (myRank == size-1) children.push_back(size);
    }

    if (reversedTree) myRank = size-1 - myRank;
    
    int exp = 1; while (exp < size) exp *= 2;
    while (true) {
        if (myRank % exp == 0) {
            int child = myRank + exp/2;
            if (child < size) {
                if (reversedTree) child = size-1 - child;
                children.push_back(child);
            } 
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

int EventDrivenBalancer::getNewDemand(int jobId) {
    return _states.getEntries().at(jobId).demand;
}

float EventDrivenBalancer::getPriority(int jobId) {
    return _states.getEntries().at(jobId).priority;
}

size_t EventDrivenBalancer::getGlobalEpoch() const {
    return _states.getGlobalEpoch();
}

robin_hood::unordered_map<int, int> EventDrivenBalancer::getBalancingResult() {

    float now = Timer::elapsedSeconds();
    
    // Mark terminated jobs
    for (const auto& [jobId, ev] : _states.getEntries()) {
        if (ev.epoch == INT_MAX) {
            log(V3_VERB, "BLC mark termination of %i\n", jobId);
            _time_of_termination[jobId] = now;
        }
    }
    // Force remove any entries associated with terminated jobs
    for (const auto& [jobId, time] : _time_of_termination) {
        _states.remove(jobId);
        _diffs.remove(jobId);
        _jobs_being_balanced.erase(jobId);
    }

    log(V5_DEBG, "BLC: calc result\n");

    int rank = MyMpi::rank(MPI_COMM_WORLD);
    int verb = rank == 0 ? V4_VVER : V5_DEBG;  

    robin_hood::unordered_map<int, int> volumes;

    // 1. Calculate aggregated demand of all jobs
    std::string assignMsg = " ";
    float aggregatedDemand = 0;
    int numJobs = 0;
    for (const auto& [key, ev] : _states.getEntries()) {
        assert(ev.demand >= 0);
        if (ev.demand == 0) continue;
        
        assert((ev.priority > 0 && ev.priority <= 1) || log_return_false("Job event for #%i has priority %.2f!\n", ev.jobId, ev.priority));

        numJobs++;
        aggregatedDemand += (ev.demand-1) * ev.priority;
        assignMsg += "#" + std::to_string(ev.jobId) + "=" + std::to_string(ev.demand) + " ";
    }
    log(verb, "BLC e=%i demand={%s}\n", _balancing_epoch, assignMsg.c_str());
    float totalAvailVolume = MyMpi::size(_comm) * _load_factor - numJobs;

    // 2a. Bail out if the elementary demand of each job cannot be met
    if (totalAvailVolume < 0) {
        log(verb, "BLC Too many jobs: bailing out, assigning 1 to each job\n");
        for (const auto& [jobId, job] : _jobs_being_balanced) {
            if (_states.getEntries().count(jobId) && getNewDemand(jobId) > 0)
                volumes[jobId] = 1;
        }
        return volumes;
    }
    
    // 2. Calculate initial assignments and remaining demanded resources
    robin_hood::unordered_map<int, double> assignments;
    float assignedResources = 0;
    std::map<float, float, std::less<float>> demandedResources;
    for (const auto& [jobId, ev] : _states.getEntries()) {
        if (ev.demand == 0) continue;

        double initialMetRatio = totalAvailVolume * ev.priority / aggregatedDemand;
        // job demand minus "atomic" demand that is met by default
        int remainingDemand = ev.demand - 1;
        // assignment: atomic node plus fair share of reduced aggregation
        assignments[jobId] = 1 + std::min(1.0, initialMetRatio) * remainingDemand;
        assignedResources += assignments[ev.jobId] - 1;
        if (!demandedResources.count(ev.priority)) demandedResources[ev.priority] = 0;
        demandedResources[ev.priority] += ev.demand - assignments[ev.jobId];
        
    }
    assignMsg = " ";
    for (const auto& [jobId, a] : assignments) {
        assert(a >= 0 || a <= totalAvailVolume || log_return_false("Invalid assignment %.3f for job %i!\n", a, jobId));
        assignMsg += "#" + std::to_string(jobId) + "=" + Logger::floatToStr(a, 2) + " ";
    }
    log(verb, "BLC e=%i init_assign={%s}\n", _balancing_epoch, assignMsg.c_str());

    // 3. Calculate final floating-point assignments for all jobs

    log(verb, "BLC e=%i init_assigned=%.3f\n", 
        _balancing_epoch, assignedResources);
    
    // Atomic job assignments are already subtracted from _total_avail_volume
    // and are not part of the all-reduced assignedResources either
    float remainingResources = totalAvailVolume - assignedResources;
    if (remainingResources < 0.1) remainingResources = 0; // too low a remainder to make a difference
    log(verb, "BLC e=%i remaining=%.3f\n", _balancing_epoch, remainingResources);

    for (const auto& [jobId, ev] : _states.getEntries()) {
        if (ev.demand <= 1) continue;

        int demand = getNewDemand(jobId);
        float priority = getPriority(jobId);
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
    }
    assignMsg = " ";
    for (const auto& e : assignments) {
        assignMsg += "#" + std::to_string(e.first) + "=" + Logger::floatToStr(e.second, 2) + " ";
    }
    log(verb, "BLC e=%i adj_assign={%s}\n", _balancing_epoch, assignMsg.c_str());

    // 4. Round job assignments
    robin_hood::unordered_map<int, int> allVolumes;
    if (_params.balanceRoundingMode() == ROUNDING_FLOOR) {
        // Round by flooring
        for (const auto& entry : _states.getEntries()) {
            allVolumes[entry.first] = std::floor(assignments[entry.first]);
        }
    } else if (_params.balanceRoundingMode() == ROUNDING_PROBABILISTIC) {
        // Round probabilistically
        for (const auto& entry : _states.getEntries()) {
            allVolumes[entry.first] = Random::roundProbabilistically(assignments[entry.first]);
        }
    } else if (_params.balanceRoundingMode() == ROUNDING_BISECTION) {

        // Calculate optimal rounding by bisection

        SortedDoubleSequence remainders;
        for (const auto& [jobId, ev] : _states.getEntries()) {
            double remainder = assignments[jobId] - (int)assignments[jobId];
            if (remainder > 0 && remainder < 1) remainders.add(remainder);
        }
        int lower = 0, upper = remainders.size();
        int idx = (lower+upper)/2;
        int iterations = 0;
        float lastUtilization = -1;

        int bestRemainderIdx = -1;
        float bestPenalty;
        float bestUtilization;

        while (true) {
            int utilization = 0;
            if (idx <= remainders.size()) {
                // Round your local assignments and calculate utilization sum
                volumes = Rounding::getRoundedAssignments(idx, utilization, remainders, assignments);
            }

            // Store result, if it is the best one so far
            float p = utilization > MyMpi::size(_comm) * _load_factor ? 1 : 
                Rounding::penalty((float)utilization / MyMpi::size(_comm), _load_factor);
            if (bestRemainderIdx == -1 || p < bestPenalty) {
                bestPenalty = p;
                bestRemainderIdx = idx;
                bestUtilization = utilization;
            }

            // Log iteration
            if (!remainders.isEmpty() && idx <= remainders.size()) {
                double remainder = (idx < remainders.size() ? remainders[idx] : 1.0);
                log(verb, "BLC e=%i RND it=%i [%i,%i]=>%i rmd=%.3f util=%.2f pen=%.2f\n", 
                                _balancing_epoch, iterations, lower, upper, idx,
                                remainder, (float)utilization, p);
            }

            // Termination?
            if (utilization == lastUtilization) { // Utilization unchanged?
                // Finished!
                int sum = 0;
                allVolumes = Rounding::getRoundedAssignments(bestRemainderIdx, sum, remainders, assignments);

                double remainder = (bestRemainderIdx < remainders.size() ? remainders[bestRemainderIdx] : 1.0);
                log(verb-1, "BLC e=%i DONE n=%i its=%i rmd=%.3f util=%.2f pen=%.2f\n", 
                            _balancing_epoch, assignments.size(), iterations, remainder, bestUtilization, bestPenalty);
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
                idx = (lower+upper)/2;
            }
            
            lastUtilization = utilization;
            iterations++;
        }
    }

    // Log final assignments
    std::string msg = "";
    int sum = 0;
    for (const auto& [jobId, vol] : allVolumes) {
        msg += " #" + std::to_string(jobId) + ":" + std::to_string(vol);
        sum += vol;
    }
    log(verb-1, "BLC assigned%s sum=%i epoch=%ld\n", msg.c_str(), sum, _states.getGlobalEpoch());

    /*
    // 5. Only remember job assignments that are of a local job
    volumes.clear();
    for (const auto& [jobId, job] : _jobs_being_balanced) {
        if (allVolumes[jobId] >= 1) volumes[jobId] = allVolumes[jobId];
    }
    */

    return volumes;
}

void EventDrivenBalancer::forget(int jobId) {
    _job_epochs.erase(jobId);
    _time_of_termination.erase(jobId);
    Balancer::forget(jobId);
}