
#pragma once

#include <algorithm>
#include <cassert>
#include <memory>
#include <mutex>
#include <set>
#include <unistd.h>

#include "comm/mympi.hpp"
#include "mpi.h"
#include "util/logger.hpp"
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
            if (status == DEPLOYED) yield(false);
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
        void yield(bool lockAlreadyHeld) {
            assert(status == DEPLOYED);
            lastStatus = status;
            status = NONE;
            parent.onDTaskRemove(*this, lockAlreadyHeld);
            if (cbEvict) {
                LOG(V4_VVER, "DTASK %i cbEvict\n", id);
                cbEvict();
            }
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
    Mutex mtx;
    std::set<DTaskSlot*, CompareDTask> evictableTasks;
    volatile int nbFreeSlots = 0;

public:
    DTaskTracker(const Parameters& params) {
        int nbJobSlots = params.jobSlots();
        int worldSize = MyMpi::size(MPI_COMM_WORLD);
        int nbWorkers = params.numWorkers() == -1 ? worldSize : params.numWorkers();
        nbFreeSlots = nbJobSlots > 0 ? std::min(nbJobSlots, nbWorkers) : nbWorkers;
    }

    std::unique_ptr<DTaskSlot> createDTask() {
        auto slot = std::unique_ptr<DTaskSlot> {new DTaskSlot(*this)};
        LOG(V5_DEBG, "DTASK %i new\n", slot->id);
        return slot;
    }

    void onDTaskDeploy(DTaskSlot& slot) {
        LOG(V5_DEBG, "DTASK %i deploy (%i free slots)\n", slot.id, nbFreeSlots);
        auto lock = mtx.getLock();
        while (nbFreeSlots == 0) {
            // Look for a task to evict
            auto it = evictableTasks.begin();
            while (it == evictableTasks.end()) {
                // None present - wait (with mutex unlocked) for one to become present
                lock.unlock();
                usleep(1000);
                lock.lock();
                it = evictableTasks.begin();
            }
            // Found a task
            {
                auto& slot = **it;
                // Obtain lock for the task's slot
                auto slotLock = slot.getLock();
                if (slot.status != DTaskSlot::DEPLOYED) {
                    // Corner case where task is not in the right state (because it's being removed!):
                    // Yield locks and sleep before retrying to find a task
                    slotLock.unlock();
                    lock.unlock();
                    usleep(1000);
                    lock.lock();
                    continue;
                }
                // Task is in the right state: Can forward "yield" signal
                // (which shortens the "evictableTasks" list with the lock already held)
                (*it)->yield(true);
            }
        }
        // Slot is free / available for this task
        assert(nbFreeSlots > 0);
        nbFreeSlots--;
    }
    void onDTaskEvictable(DTaskSlot& slot, float timeToAdd) {
        LOG(V5_DEBG, "DTASK %i evictable\n", slot.id);
        auto lock = mtx.getLock();
        evictableTasks.erase(&slot);
        slot.totalActiveTime += timeToAdd;
        evictableTasks.insert(&slot);
    }
    void onDTaskNonEvictable(DTaskSlot& slot) {
        LOG(V5_DEBG, "DTASK %i nonevictable\n", slot.id);
        auto lock = mtx.getLock();
        evictableTasks.erase(&slot);
    }
    void onDTaskRemove(DTaskSlot& slot, bool lockAlreadyHeld) {
        if (!lockAlreadyHeld) mtx.lock();
        LOG(V5_DEBG, "DTASK %i remove (%i free slots)\n", slot.id, nbFreeSlots);
        evictableTasks.erase(&slot);
        nbFreeSlots++;
        if (!lockAlreadyHeld) mtx.unlock();
    }
};
