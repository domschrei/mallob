
#pragma once

#include <functional>
#include <list>
#include <set>

#include "comm/mympi.hpp"
#include "util/assert.hpp"
#include "util/logger.hpp"
#include "util/params.hpp"
#include "util/sys/timer.hpp"

class JobSlotRegistry {

public:
    struct JobSlot {
    private:
        std::string name;
        bool allocated {false};
        float timeOfStart {0};
        float timeOfLastActiveStart {-1};
        float totalActiveTime {0};
        std::function<void()> callbackAtEviction;
    public:
        JobSlot(const std::string& name, std::function<void()> cb) : name(name), callbackAtEviction(cb) {
            timeOfStart = Timer::elapsedSeconds();
        }
        JobSlot(JobSlot&& other) {
            *this = std::move(other);
        }
        JobSlot& operator=(JobSlot&& other) {
            name = other.name;
            allocated = other.allocated;
            callbackAtEviction = other.callbackAtEviction;
            other.name = {};
            other.allocated = false;
            other.callbackAtEviction = {};
            return *this;
        }
        void startActiveTime() {
            timeOfLastActiveStart = Timer::elapsedSeconds();
        }
        void endActiveTime() {
            if (timeOfLastActiveStart >= 0)
                totalActiveTime += Timer::elapsedSeconds() - timeOfLastActiveStart;
            timeOfLastActiveStart = -1;
        }
        const char* getName() const {
            return name.c_str();
        }
        float getTimeOfStart() const {
            return timeOfStart;
        }
        float getTotalActiveTime() const {
            return totalActiveTime;
        }
        bool isAllocated() const {
            return allocated;
        }
        void acquire() {
            assert(!allocated);
            allocated = true;
        }
        void evict() {
            if (allocated) {
                totalActiveTime = 0;
                callbackAtEviction();
            }
            allocated = false;
        }
        void release() {
            allocated = false;
        }
    };

private:
    struct JobSlotCompare {
        bool operator()(const std::shared_ptr<JobSlot>& left, const std::shared_ptr<JobSlot>& right) const {
            if (left->isAllocated() != right->isAllocated())
                return !left->isAllocated() && right->isAllocated();
            if (left->getTotalActiveTime() != right->getTotalActiveTime())
                return left->getTotalActiveTime() < right->getTotalActiveTime();
            return left.get() < right.get();
        }
    };
    static std::set<std::shared_ptr<JobSlot>, JobSlotCompare> _slots;
    static int _max_nb_slots;

public:
    static bool isInitialized() {
        return _max_nb_slots > 0;
    }
    static void init(const Parameters& params) {
        int worldSize = MyMpi::size(MPI_COMM_WORLD);
        _max_nb_slots = params.jobSlots() > 0 ? std::min(params.jobSlots(), worldSize) : worldSize;
    }
    static void acquireSlot(std::shared_ptr<JobSlot> slot) {
        assert(isInitialized());
        slot->acquire();
        auto [itNew, ok] = _slots.insert(slot);
        assert(ok);

        if (_slots.size() > _max_nb_slots) {
            auto it = _slots.begin();
            while (it == itNew) ++it;
            assert(it != _slots.end());
            LOG(V3_VERB, "Evict %s alloc=%i timeofstart=%.3f totaltime=%.3f\n",
                (*it)->getName(), (*it)->isAllocated(), (*it)->getTimeOfStart(), (*it)->getTotalActiveTime());
            (*it)->evict(); // no-op if it was already released
            _slots.erase(it);
        }
    }
};
