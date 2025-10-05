
#pragma once

#include <functional>
#include <list>

#include "comm/mympi.hpp"
#include "util/assert.hpp"
#include "util/params.hpp"

class JobSlotRegistry {

public:
    struct JobSlot {
        std::function<void()> callbackAtEviction;
        JobSlot(std::function<void()> cb) : callbackAtEviction(cb) {}
        ~JobSlot() {
            if (callbackAtEviction) callbackAtEviction();
        }
    };

private:
    static std::list<JobSlot> _slots;
    static int _max_nb_slots;

public:
    static bool isInitialized() {
        return _max_nb_slots > 0;
    }
    static void init(const Parameters& params) {
        _max_nb_slots = params.jobSlots() > 0 ? params.jobSlots() : MyMpi::size(MPI_COMM_WORLD);
    }
    static void acquireSlot(std::function<void()> cbAtEviction) {
        assert(isInitialized());
        _slots.push_back(cbAtEviction);
        if (_slots.size() > _max_nb_slots)
            _slots.pop_front(); // triggers eviction callback
    }
};
