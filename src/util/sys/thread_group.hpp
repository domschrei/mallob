
#ifndef DOMPASCH_MALLOB_THREAD_GROUP_HPP
#define DOMPASCH_MALLOB_THREAD_GROUP_HPP

#include <vector>
#include <thread>
#include <functional>

#include "util/sys/threading.hpp"

class ThreadGroup {

private:
    std::vector<std::thread> _threads;
    std::vector<bool> _threads_running;
    Mutex _vec_manip_mutex;

public:
    ThreadGroup() {}
    void doTask(std::function<void()> callable) {
        cleanUp();
        auto lock = _vec_manip_mutex.getLock();
        const size_t i = _threads.size();
        _threads_running.push_back(true);
        _threads.emplace_back([this, callable, i]() {
            callable();
            auto l = _vec_manip_mutex.getLock();
            _threads_running[i] = false;
        });
    }
    void cleanUp() {
        auto lock = _vec_manip_mutex.getLock();
        for (const auto& running : _threads_running) 
            if (running) return;
        
        _threads_running.clear();
        for (auto& thread : _threads) thread.join();
        _threads.clear();
    }
    ~ThreadGroup() {
        auto lock = _vec_manip_mutex.getLock();
        for (auto& thread : _threads) if (thread.joinable()) thread.join();
    }
};


#endif