
#ifndef DOMPASCH_MALLOB_THREAD_GROUP_HPP
#define DOMPASCH_MALLOB_THREAD_GROUP_HPP

#include <array>
#include <list>
#include <thread>
#include <functional>

#include "util/sys/threading.hpp"

template<typename T = void*>
class ThreadGroup {

private:
    bool _exiting = false;
    std::vector<std::thread> _threads;
    std::vector<T>& _bundles;
    std::vector<std::thread> _threads_running;
    std::list<std::function<void(T&)>> _open_tasks;
    Mutex _open_tasks_manip_mutex;

public:
    ThreadGroup(size_t numThreads, std::vector<T>& bundlePerThread = std::vector<T>())
            : _bundles(bundlePerThread) {

        for (size_t i = 0; i < numThreads; i++) {
            _threads.emplace_back([this, i]() {
                while (!_exiting) {
                    if (!_open_tasks_manip_mutex.tryLock()) {
                        usleep(1000);
                    } else {
                        // Mutex locked
                        std::optional<std::function<void(T&)>> task;
                        if (!_open_tasks.empty()) {
                            task = std::move(_open_tasks.front());
                            _open_tasks.erase(_open_tasks.begin());
                        }
                        _open_tasks_manip_mutex.unlock();
                        // Mutex unlocked
                        if (task.has_value()) task.value()(_bundles[i]);
                    }
                }
            });
        }
    }
    void doTask(std::function<void(T&)> callable) {
        if (_exiting) return;
        auto lock = _open_tasks_manip_mutex.getLock();
        _open_tasks.push_back(callable);
    }
    ~ThreadGroup() {
        _exiting = true;
        for (auto& thread : _threads) if (thread.joinable()) thread.join();
    }
};


#endif