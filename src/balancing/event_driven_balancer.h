
#ifndef DOMPASCH_BALANCER_EVENT_DRIVEN_H
#define DOMPASCH_BALANCER_EVENT_DRIVEN_H

#include <utility>
#include <map>

#include "balancing/balancer.h"
#include "data/reduceable.h"
#include "util/console.h"

struct Event {
    int jobId;
    int epoch;
    int demand;
    float priority;
};

class EventMap : public Reduceable {

private:
    std::map<int, Event> _map;
    const int _size_per_event = 3*sizeof(int)+sizeof(float);
public:
    virtual std::shared_ptr<std::vector<uint8_t>> serialize() const override {
        auto result = std::make_shared<std::vector<uint8_t>>(_map.size() * _size_per_event);
        int i = 0, n;
        for (const auto& entry : _map) {
            n = sizeof(int); memcpy(result->data()+i, &entry.second.jobId, n); i += n;
            n = sizeof(int); memcpy(result->data()+i, &entry.second.epoch, n); i += n;
            n = sizeof(int); memcpy(result->data()+i, &entry.second.demand, n); i += n;
            n = sizeof(float); memcpy(result->data()+i, &entry.second.priority, n); i += n;
        }
        return result;
    }
    virtual void deserialize(const std::vector<uint8_t>& packed) override {
        assert(packed.size() % _size_per_event == 0);
        _map.clear();
        int numEvents = packed.size() / _size_per_event;
        int i = 0, n;
        for (int ev = 0; ev < numEvents; ev++) {
            Event newEvent;
            n = sizeof(int); memcpy(&newEvent.jobId, packed.data()+i, n); i += n;
            n = sizeof(int); memcpy(&newEvent.epoch, packed.data()+i, n); i += n;
            n = sizeof(int); memcpy(&newEvent.demand, packed.data()+i, n); i += n;
            n = sizeof(float); memcpy(&newEvent.priority, packed.data()+i, n); i += n;
            _map[newEvent.jobId] = newEvent;
        }
    }
    virtual void merge(const Reduceable& other) {

        EventMap& otherEventMap = (EventMap&) other;
        auto it = _map.begin();
        auto otherIt = otherEventMap._map.begin();
        std::map<int, Event> newMap;

        // Iterate over both event maps (sorted by job ID) simultaneously
        while (it != _map.end() || otherIt != otherEventMap._map.end()) {

            if (it != _map.end() && otherIt != otherEventMap._map.end()) {
                // Both have an element left: compare them
                const auto& pair = *it;
                const auto& otherPair = *otherIt;
                if (pair.first == otherPair.first) {
                    // Same ID -- take newer event, forget other one
                    newMap[pair.first] = (pair.second.epoch >= otherPair.second.epoch ? pair.second : otherPair.second);
                    it++; otherIt++;
                } else {
                    // Different ID -- insert lower one
                    if (pair.first < otherPair.first) {
                        newMap[pair.first] = pair.second;
                        it++;
                    } else {
                        newMap[otherPair.first] = otherPair.second;
                        otherIt++;
                    }
                }
            } else if (it != _map.end()) {
                // only "it" has an element left: insert
                newMap[it->first] = it->second;
                it++;
            } else {
                // only "otherIt" has an element left: insert
                newMap[otherIt->first] = otherIt->second;
                otherIt++;
            }
        }

        _map = newMap;
    }
    virtual std::unique_ptr<Reduceable> getDeserialized(const std::vector<uint8_t>& packed) const {
        auto result = std::unique_ptr<Reduceable>(new EventMap());
        result->deserialize(packed);
        return result;
    }
    virtual bool isEmpty() {
        return _map.empty();
    }

    bool insertIfNovel(const Event& ev) {
        // Update map if no such job entry yet or existing entry is older
        if (!_map.count(ev.jobId) || _map[ev.jobId].epoch < ev.epoch) {
            _map[ev.jobId] = ev;
            return true;
        }
        return false;
    }
    const std::map<int, Event>& getEntries() const {
        return _map;
    }
    void filterBy(const EventMap& otherMap) {
        std::vector<int> keysToErase;
        for (const auto& entry : _map) {
            if (otherMap.getEntries().count(entry.first) 
                && otherMap.getEntries().at(entry.first).epoch >= entry.second.epoch) {
                // Filtered out
                keysToErase.push_back(entry.first);
            }
        }
        for (auto key : keysToErase) _map.erase(key);
    }
    bool updateBy(const EventMap& otherMap) {
        bool change = false;
        for (const auto& entry : otherMap.getEntries()) {
            change |= insertIfNovel(entry.second);
        }
        return change;
    }
    void removeOldZeros() {
        int epochDiff = 100;
        int latestEpoch = 0;
        std::vector<int> keysToErase;
        for (const auto& entry : _map) {
            if (entry.second.demand == 0) {
                // Filtered out
                keysToErase.push_back(entry.first);
            }
            latestEpoch = std::max(latestEpoch, entry.second.epoch);
        }
        // Remove all filtered keys which are old enough
        for (auto key : keysToErase) {
            if (latestEpoch - _map[key].epoch >= epochDiff) _map.erase(key);
        }
    }
};

class EventDrivenBalancer : public Balancer {

public:
    EventDrivenBalancer(MPI_Comm& comm, Parameters& params, Statistics& stats) : Balancer(comm, params, stats) {}

    bool beginBalancing(std::map<int, Job*>& jobs) override {
        // Initialize
        _balancing = true;
        _balancing_epoch++;

        // Identify jobs to balance
        int numActiveJobs = 0;

        _jobs_being_balanced = std::map<int, Job*>();
        for (auto it : jobs) {
            bool isActiveRoot = it.second->isRoot() && it.second->isNotInState({INITIALIZING_TO_PAST}) 
                                && (it.second->isInState({ACTIVE, STANDBY}) || it.second->isInitializing());
            // Node must be root node to participate
            bool participates = it.second->isRoot();
            // Job must be active, or must be initializing and already having the description
            participates &= it.second->isInState({JobState::ACTIVE, JobState::STANDBY})
                            || (it.second->isInState({JobState::INITIALIZING_TO_ACTIVE}) 
                                && it.second->hasJobDescription());
            if (participates || isActiveRoot) {
                // Job participates
                _jobs_being_balanced[it.first] = it.second;

                // Insert this job as an event, if there is something novel about it
                if (!_job_epochs.count(it.first)) _job_epochs[it.first] = 1;
                int epoch = _job_epochs[it.first];
                int demand = getDemand(*_jobs_being_balanced[it.first]);
                Event ev({it.first, epoch, demand, _jobs_being_balanced[it.first]->getDescription().getPriority()});
                if (!_states.getEntries().count(it.first) || _states.getEntries().at(it.first).demand != demand) {
                    // Not contained yet in state: try to insert into diffs map
                    bool inserted = _diffs.insertIfNovel(ev);
                    if (inserted) _job_epochs[it.first]++;
                }

                numActiveJobs++;
            }
        }

        // initiate a balancing, if applicable
        reduceIfApplicable(BOTH);
        return false;
    }
    bool canContinueBalancing() override {return false;}
    bool continueBalancing() override {return false;}
    bool continueBalancing(MessageHandlePtr handle) override {return this->handle(handle);}
    std::map<int, int> getBalancingResult() override {return _balancing_result;}

private:
    EventMap _states;
    EventMap _diffs;
    std::map<int, int> _job_epochs;
    float _last_balancing;

    std::map<int, int> _balancing_result;

    const int NORMAL_TREE = 1, REVERSED_TREE = 2, BOTH = 3;

    bool reduceIfApplicable(int which) {
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

    bool reduce(const EventMap& data, bool reversedTree) {
        bool done = false;

        int parent = getParentRank(reversedTree);
        if (parent == MyMpi::rank(MPI_COMM_WORLD)) {

            // No parent / I AM ROOT. 
            
            // Send to other root
            MyMpi::isend(MPI_COMM_WORLD, getRootRank(!reversedTree), MSG_ANYTIME_REDUCTION, data);
            
            // Broadcast and digest
            broadcast(data, reversedTree);
            done = digest(data);

        } else {

            // Send to actual parent
            MyMpi::isend(MPI_COMM_WORLD, parent, MSG_ANYTIME_REDUCTION, data);
        }

        return done;     
    }

    void broadcast(const EventMap& data, bool reversedTree) {

        int child = getChildRank(reversedTree);
        if (child != MyMpi::rank(MPI_COMM_WORLD)) {
            // Send to actual child
            MyMpi::isend(MPI_COMM_WORLD, getChildRank(reversedTree), MSG_ANYTIME_BROADCAST, data);
        }
    }

    bool handle(const MessageHandlePtr& handle) {
        int sender = handle->source;
        int myRank = MyMpi::rank(MPI_COMM_WORLD);
        EventMap data; data.deserialize(*(handle->recvData));
        bool done;

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

    bool digest(const EventMap& data) {

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

    int getRootRank(bool reversedTree) {
        if (reversedTree) return MyMpi::size(_comm)-1;
        return 0;
    }
    int getParentRank(bool reversedTree) {
        int myRank = MyMpi::rank(MPI_COMM_WORLD);
        if (reversedTree) myRank = MyMpi::size(_comm) - myRank;
        
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

        if (reversedTree) parent = MyMpi::size(_comm) - parent;
        return parent;
    }
    int getChildRank(bool reversedTree) {
        int myRank = MyMpi::rank(MPI_COMM_WORLD);
        if (reversedTree) myRank = MyMpi::size(_comm) - myRank;
        
        int child;
        int exp = 2;
        if (myRank % exp == 1) child = myRank;
        else while (true) {
            if (myRank % (2*exp) == exp) {
                child = myRank + exp/2;
                break;
            }
            exp *= 2;
        }

        if (reversedTree) child = MyMpi::size(_comm) - child;
        return child;
    }
    bool isRoot(int rank, bool reversedTree) {
        return rank == getRootRank(reversedTree);
    }
    bool isLeaf(int rank, bool reversedTree) {
        return rank % 2 == (reversedTree ? 0 : 1);
    }

    void calculateBalancingResult() {

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
};

#endif