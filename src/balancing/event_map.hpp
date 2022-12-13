
#ifndef DOMPASCH_MALLOB_EVENT_MAP_HPP
#define DOMPASCH_MALLOB_EVENT_MAP_HPP

#include <map>
#include <vector>
#include <memory>

#include "data/reduceable.hpp"
#include "util/logger.hpp"

struct Event {
    int jobId;
    int epoch;
    int demand;
    float priority;

    // only for balancing - not serialized in EventMap serialization
    double assignment;
    double volume;

    bool operator==(const Event& other) const {
        return jobId == other.jobId && epoch == other.epoch 
                && demand == other.demand && priority == other.priority;
    }

    bool operator!=(const Event& other) const {
        return !(*this == other);
    }

    bool dominates(const Event& other) const {
        return epoch > other.epoch;
    }
};

class EventMap : public Reduceable {

private:
    size_t _global_epoch = 0;
    std::map<int, Event> _map;

    const int _size_per_event = 3*sizeof(int)+sizeof(float);

public:
    virtual std::vector<uint8_t> serialize() const override {
        std::vector<uint8_t> result(sizeof(size_t) + _map.size() * _size_per_event);
        int i = 0, n;
        n = sizeof(size_t); memcpy(result.data()+i, &_global_epoch, n); i += n;
        for (const auto& entry : _map) {
            n = sizeof(int); memcpy(result.data()+i, &entry.second.jobId, n); i += n;
            n = sizeof(int); memcpy(result.data()+i, &entry.second.epoch, n); i += n;
            n = sizeof(int); memcpy(result.data()+i, &entry.second.demand, n); i += n;
            n = sizeof(float); memcpy(result.data()+i, &entry.second.priority, n); i += n;
        }
        return result;
    }
    virtual EventMap& deserialize(const std::vector<uint8_t>& packed) override {
        _map.clear();
        int numEvents = (packed.size()-sizeof(size_t)) / _size_per_event;
        int i = 0, n;
        n = sizeof(size_t); memcpy(&_global_epoch, packed.data()+i, n); i += n;
        if (packed.size() <= sizeof(size_t)+sizeof(int)) return *this;
        for (int ev = 0; ev < numEvents; ev++) {
            Event newEvent;
            n = sizeof(int); memcpy(&newEvent.jobId, packed.data()+i, n); i += n;
            n = sizeof(int); memcpy(&newEvent.epoch, packed.data()+i, n); i += n;
            n = sizeof(int); memcpy(&newEvent.demand, packed.data()+i, n); i += n;
            n = sizeof(float); memcpy(&newEvent.priority, packed.data()+i, n); i += n;
            _map[newEvent.jobId] = newEvent;
        }
        return *this;
    }
    virtual void aggregate(const Reduceable& other) {

        EventMap& otherEventMap = (EventMap&) other;
        _global_epoch = std::max(_global_epoch, otherEventMap._global_epoch);
        auto it = _map.begin();
        auto otherIt = otherEventMap._map.begin();
        std::map<int, Event> newMap;

        // Iterate over both event maps (sorted by job ID) simultaneously
        while (it != _map.end() || otherIt != otherEventMap._map.end()) {

            if (it != _map.end() && otherIt != otherEventMap._map.end()) {
                // Both have an element left: compare them
                const auto& [id, ev] = *it;
                const auto& [otherId, otherEv] = *otherIt;
                if (id == otherId) {
                    // Same ID -- take newer event, forget other one
                    newMap[id] = (ev.dominates(otherEv) ? ev : otherEv);
                    it++; otherIt++;
                } else {
                    // Different ID -- insert lower one
                    if (id < otherId) {
                        newMap[id] = ev;
                        it++;
                    } else {
                        newMap[otherId] = otherEv;
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
    virtual bool isEmpty() const {
        return _map.empty();
    }
    size_t getGlobalEpoch() const {
        return _global_epoch;
    }
    void setGlobalEpoch(int epoch) {
        _global_epoch = epoch;
    }

    bool insertIfNovel(const Event& ev) {
        if (ev.epoch < 0) return false; // Old, terminated job
        // Update map if no such job entry yet or existing entry is older
        if (!_map.count(ev.jobId) || ev.dominates(_map[ev.jobId])) {
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
        for (const auto& [jobId, ev] : _map) {
            if (otherMap.getEntries().count(jobId)) {
                auto& otherEv = otherMap.getEntries().at(jobId);
                if (otherEv.epoch == ev.epoch) {
                    if (otherEv.priority != ev.priority) {
                        LOG(V0_CRIT, "[ERROR] #%i e=%i : prio %.2f != %.2f!\n", jobId, ev.epoch, ev.priority, otherEv.priority);
                        abort();
                    }
                    if (otherEv.demand != ev.demand) {
                        LOG(V0_CRIT, "[ERROR] #%i e=%i : demand %i != %i!\n", jobId, ev.epoch, ev.demand, otherEv.demand);
                        abort();
                    }
                }
                if (otherEv.epoch >= ev.epoch) {
                    // Filtered out
                    keysToErase.push_back(jobId);
                }
            }
        }
        for (auto key : keysToErase) _map.erase(key);
        _global_epoch = std::max(_global_epoch, otherMap._global_epoch);
    }
    bool updateBy(const EventMap& otherMap) {
        bool change = false;
        for (const auto& entry : otherMap.getEntries()) {
            change |= insertIfNovel(entry.second);
        }
        change |= otherMap._global_epoch > _global_epoch;
        _global_epoch = std::max(_global_epoch, otherMap._global_epoch);
        return change;
    }
    std::vector<int> removeOldZeros() {      
        // Remove entries for which demand and priority are set to zero
        std::vector<int> keysToErase;
        for (const auto& [jobId, ev] : _map) {
            if (ev.demand == 0 && ev.priority <= 0) {
                // Filtered out
                keysToErase.push_back(jobId);
            }
        }
        for (auto key : keysToErase) _map.erase(key);
        return keysToErase;
    }
    void remove(int key) {
        _map.erase(key);
    }
    void clear() {
        _map.clear();
    }
    bool operator==(const EventMap& other) const {
        return getEntries() == other.getEntries();
    }
    bool operator!=(const EventMap& other) const {
        return !(*this == other);
    }

    std::string toStr() const {
        std::string out = "{ ";
        for (auto& [id, ev] : getEntries()) {
            out += "(" + std::to_string(id) + "," + std::to_string(ev.demand) + "," 
                + std::to_string(ev.priority) + "," + std::to_string(ev.epoch) + ") ";
        }
        out += "}";
        return out;
    }
};

#endif
