
#pragma once

#include <list>
#include <memory>

#include "util/sys/threading.hpp"
#include "wrapped_sat_job_stream.hpp"

struct SatJobStreamGarbageCollector {
    Mutex mtxGarbage;
    ConditionVariable condVarGarbage;
    std::list<std::unique_ptr<WrappedSatJobStream>> garbage;
    volatile bool stop {false};
    std::thread bgThread;
    SatJobStreamGarbageCollector() {
        bgThread = std::thread([this]() {run();});
    }
    void run() {
        while (true) {
            std::unique_ptr<WrappedSatJobStream> item;
            {
                auto lock = mtxGarbage.getLock();
                condVarGarbage.waitWithLockedMutex(lock, [&]() {
                    return !garbage.empty() || stop;
                });
                if (garbage.empty()) break;
                item = std::move(garbage.front());
                garbage.pop_front();
            }
            item.reset(); // delete garbage
        }
    }
    void add(std::unique_ptr<WrappedSatJobStream>&& item) {
        {
            auto lock = mtxGarbage.getLock();
            garbage.push_back(std::move(item));
        }
        condVarGarbage.notify();
    }
    ~SatJobStreamGarbageCollector() {
        stop = true;
        add({});
        bgThread.join();
    }
    static SatJobStreamGarbageCollector& get() {return *_gc;}
private:
    static SatJobStreamGarbageCollector* _gc;
};
