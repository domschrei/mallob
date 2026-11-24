
#pragma once

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <functional>
#include <unistd.h>

#include "comm/mympi.hpp"
#include "mpi.h"
#include "util/logger.hpp"
#include "util/params.hpp"
#include "util/sys/threading.hpp"
#include "util/sys/timer.hpp"

class DTaskTracker {

public:
    struct DTask {
        volatile bool evicted {false};
        volatile float totalActiveTime {0};
        volatile float startOfCurrentActiveTime {-1};
        void startActiveTime() {
            commitActiveTime();
            startOfCurrentActiveTime = Timer::elapsedSeconds();
        }
        void commitActiveTime() {
            if (startOfCurrentActiveTime >= 0) {
                totalActiveTime += (Timer::elapsedSeconds() - startOfCurrentActiveTime);
                startOfCurrentActiveTime = -1;
            }
        }
    };

private:
    Mutex mtx;
    std::vector<std::shared_ptr<DTask>> tasks;
    volatile int nbTotalSlots = 0;

public:
    DTaskTracker(const Parameters& params) {
        int nbJobSlots = params.jobSlots();
        int worldSize = MyMpi::size(MPI_COMM_WORLD);
        int nbWorkers = params.numWorkers() == -1 ? worldSize : params.numWorkers();
        nbTotalSlots = nbJobSlots > 0 ? std::min(nbJobSlots, nbWorkers) : nbWorkers;
    }

    std::shared_ptr<DTask> acquireSlot() {
        auto lock = mtx.getLock();

        if (tasks.size() == nbTotalSlots) {
            // none left: evict a task
            int minPos = -1;
            float minTime = INT32_MAX;
            for (int i = 0; i < tasks.size(); i++) {
                auto& t = tasks[i];
                if (t->evicted) {
                    // Was evicted from the task's owner themselves,
                    // so we can always choose this one
                    minPos = i;
                    break;
                }
                if (t->totalActiveTime < minTime) {
                    minPos = i;
                    minTime = t->totalActiveTime;
                }
            }
            assert(minPos >= 0);
            // Evict and remove task
            tasks[minPos]->evicted = true;
            tasks[minPos] = tasks[tasks.size() -1];
            tasks.resize(tasks.size() - 1);
        }

        std::shared_ptr<DTask> task (new DTask());
        tasks.push_back(task);
        return task;
    }
};
