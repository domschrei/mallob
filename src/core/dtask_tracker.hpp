
#pragma once

#include <algorithm>
#include <cassert>
#include <memory>
#include <mutex>
#include <set>

#include "comm/mympi.hpp"
#include "mpi.h"
#include "util/params.hpp"
#include "util/sys/threading.hpp"
#include "util/sys/timer.hpp"

class DTaskTracker {

public:
    struct DTaskSlot {
        int id;
        DTaskTracker& parent;
        enum Status {NONE, DEPLOYED, ACTIVE} status;
        Status lastStatus;
        float timeOfStart;
        float totalActiveTime {0};
        Mutex mtx;
        std::function<void()> cbEvict;

        DTaskSlot(DTaskTracker& parent) : parent(parent), status(NONE), lastStatus(status) {
            static int runningId = 1;
            id = runningId++;
        }
        ~DTaskSlot() {
            // If we're cleaning up, nothing concurrent should happen any more,
            // so we don't put a lock around these top level checks.
            if (status == ACTIVE) suspend();
            if (status == DEPLOYED) yield();
        }

        void setCallbackOnEvict(std::function<void()> cb) {
            cbEvict = cb;
        }

        void deploy() {
            assert(status == NONE);
            lastStatus = status;
            status = DEPLOYED;
            parent.onDTaskDeploy(*this);
        }
        void resume() {
            assert(status == DEPLOYED);
            lastStatus = status;
            status = ACTIVE;
            timeOfStart = Timer::elapsedSeconds();
            parent.onDTaskNonEvictable(*this);
        }
        void suspend() {
            assert(status == ACTIVE);
            lastStatus = status;
            status = DEPLOYED;
            parent.onDTaskEvictable(*this, Timer::elapsedSeconds() - timeOfStart);
        }
        void yield() {
            assert(status == DEPLOYED);
            lastStatus = status;
            status = NONE;
            parent.onDTaskRemove(*this);
            if (cbEvict) cbEvict();
        }
        bool wasEvicted() {
            return lastStatus == DEPLOYED && status == NONE;
        }
        std::unique_lock<std::mutex> getLock() {
            return mtx.getLock();
        }
    };
    struct CompareDTask {
        bool operator()(const DTaskSlot* left, const DTaskSlot* right) const {
            if (left->totalActiveTime != right->totalActiveTime)
                return left->totalActiveTime < right->totalActiveTime;
            return left->id < right->id;
        }
    };

private:
    std::set<DTaskSlot*, CompareDTask> evictableTasks;
    int nbFreeSlots = 0;

public:
    DTaskTracker(const Parameters& params) {
        int nbJobSlots = params.jobSlots();
        int worldSize = MyMpi::size(MPI_COMM_WORLD);
        int nbWorkers = params.numWorkers() == -1 ? worldSize : params.numWorkers();
        nbFreeSlots = nbJobSlots > 0 ? std::min(nbJobSlots, nbWorkers) : nbWorkers;
    }

    std::unique_ptr<DTaskSlot> createDTask() {
        return std::unique_ptr<DTaskSlot> {new DTaskSlot(*this)};
    }

    void onDTaskDeploy(DTaskSlot& slot) {
        if (nbFreeSlots == 0) {
            auto it = evictableTasks.begin();
            assert(it != evictableTasks.end());
            assert((*it)->status == DTaskSlot::DEPLOYED);
            (*it)->yield();
        }
        assert(nbFreeSlots > 0);
        nbFreeSlots--;
    }
    void onDTaskEvictable(DTaskSlot& slot, float timeToAdd) {
        evictableTasks.erase(&slot);
        slot.totalActiveTime += timeToAdd;
        evictableTasks.insert(&slot);
    }
    void onDTaskNonEvictable(DTaskSlot& slot) {
        evictableTasks.erase(&slot);
    }
    void onDTaskRemove(DTaskSlot& slot) {
        evictableTasks.erase(&slot);
        nbFreeSlots++;
    }
};
