
#pragma once

#include <functional>
#include <list>

#include "comm/mympi.hpp"
#include "util/assert.hpp"
#include "util/params.hpp"

class JobSlotRegistry {

public:
    struct JobSlot {
        bool allocated {false};
        std::function<void()> callbackAtEviction;
        JobSlot(std::function<void()> cb) : callbackAtEviction(cb) {}
        JobSlot(JobSlot&& other) {
            *this = std::move(other);
        }
        JobSlot& operator=(JobSlot&& other) {
            allocated = other.allocated;
            callbackAtEviction = other.callbackAtEviction;
            other.allocated = false;
            other.callbackAtEviction = {};
            return *this;
        }
        void evict() {
            if (allocated) callbackAtEviction();
            allocated = false;
        }
    };

private:
    static std::list<JobSlot*> _slots;
    static int _max_nb_slots;

public:
    static bool isInitialized() {
        return _max_nb_slots > 0;
    }
    static void init(const Parameters& params) {
        _max_nb_slots = params.jobSlots() > 0 ? params.jobSlots() : MyMpi::size(MPI_COMM_WORLD);
    }
    static void acquireSlot(JobSlot& slot) {
        assert(isInitialized());
        slot.allocated = true;
        _slots.push_back(&slot);
        if (_slots.size() > _max_nb_slots) {
            _slots.front()->evict();
            _slots.pop_front();
        }
    }
};
