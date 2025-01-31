
#ifndef DOMPASCH_MALLOB_THREAD_POOL_HPP
#define DOMPASCH_MALLOB_THREAD_POOL_HPP

#include <algorithm>
#include <atomic>
#include <ext/alloc_traits.h>
#include <stdlib.h>
#include <list>
#include <thread>
#include <future>
#include <functional>
#include <memory>
#include <string>
#include <utility>

#include "util/sys/process.hpp"
#include "util/sys/threading.hpp"
#include "util/logger.hpp"
#include "util/sys/proc.hpp"
#include "util/sys/timer.hpp"

class ThreadPool {

public:
    struct Runnable {
        std::function<void()> function;
        std::promise<void> promise;
    };

private:
    std::thread _checker_thread;
    volatile pthread_t _checker_thread_tid {0};

    std::list<std::thread> _threads;
    std::list<Runnable> _job_queue;
    Mutex _job_queue_mutex;
    ConditionVariable _job_queue_cond_var;
    volatile bool _terminate = false;

    std::atomic_int _nb_pending_tasks {0};
    float _last_enqueue {0};

public:
    ThreadPool(size_t size) : _threads(size) {
        for (size_t i = 0; i < size; i++) {
            _threads.emplace_back([&, i]() {runThread(i);});
        }
        _checker_thread = std::thread([&]() {
            _checker_thread_tid = Process::getPthreadId();
            while (!_terminate) {
                if (_nb_pending_tasks.load(std::memory_order_relaxed) > 0)
                    resizeIfNeeded();
                usleep(1000*100); // will be interrupted as needed
            }
        });
        while (_checker_thread_tid == 0) {} // wait until initialized
    }
    ~ThreadPool() {
        _terminate = true;
        Process::wakeUpThread(_checker_thread_tid);
        for (auto& thread : _threads) thread.join();
        _checker_thread.join();
    }

    std::future<void> addTask(std::function<void()>&& function) {
        Runnable r;
        r.function = std::move(function);
        std::future<void> future;
        {
            auto lock = _job_queue_mutex.getLock();
            _job_queue.push_back(std::move(r));
            future = _job_queue.back().promise.get_future();
            _last_enqueue = Timer::elapsedSeconds();
            _nb_pending_tasks++;
        }
        _job_queue_cond_var.notify();
        Process::wakeUpThread(_checker_thread_tid);
        return future;
    }

    void resizeIfNeeded() {
        auto lock = _job_queue_mutex.getLock();
        if (_job_queue.empty()) return;
        if (Timer::elapsedSeconds()-_last_enqueue < 0.1f) return;
        // a task has been pending for at least 0.1s
        int priorSize = _threads.size();
        int newSize = std::max(priorSize+1, (int) (priorSize*1.25));
        LOG(V4_VVER, "Grow thread pool (%i -> %i)\n", priorSize, newSize);
        while (_threads.size() < newSize)
            _threads.emplace_back([&, i=_threads.size()]() {runThread(i);});
    }

private:
    void runThread(int id) {
        std::string threadName = "ThreadPool#" + std::to_string(id);
        Proc::nameThisThread(threadName.c_str());

        Runnable r;
        while (!_terminate) {
            {
                auto lock = _job_queue_mutex.getLock();
                _job_queue_cond_var.waitWithLockedMutex(lock, [&]() {return !_job_queue.empty();});
                r = std::move(_job_queue.front());
                _job_queue.pop_front();
                _nb_pending_tasks--;
            }
            r.function();
            r.promise.set_value();
        }
    }
};

class ProcessWideThreadPool {
private:
    static ThreadPool* pool;
public:
    static void init(size_t size) {pool = new ThreadPool(size);}
    static ThreadPool& get() {
        if (pool == nullptr) {
            LOG(V0_CRIT, "[ERROR] Process-wide thread pool was requested, but is not initialized!\n");
            exit(1);
        }
        return *pool;
    }  
};

#endif
