
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
    EventDrivenBalancer(MPI_Comm& comm, Parameters& params, Statistics& stats) : Balancer(comm, params, stats) {
        _last_balancing = 0;
    }

    bool beginBalancing(std::map<int, Job*>& jobs) override {
        // Initialize
        //_balancing = true;
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

    bool handle(const MessageHandlePtr& handle);
    bool reduce(const EventMap& data, bool reversedTree);
    bool reduceIfApplicable(int which);
    void broadcast(const EventMap& data, bool reversedTree);
    bool digest(const EventMap& data);

    int getRootRank(bool reversedTree);
    int getParentRank(bool reversedTree);
    int getChildRank(bool reversedTree);
    bool isRoot(int rank, bool reversedTree);
    bool isLeaf(int rank, bool reversedTree);

    void calculateBalancingResult();
};

#endif