
#ifndef DOMPASCH_MALLOB_THREAD_POOL_HPP
#define DOMPASCH_MALLOB_THREAD_POOL_HPP

#include <vector>
#include <list>
#include <thread>
#include <future>

#include "util/sys/threading.hpp"
#include "util/logger.hpp"
#include "util/sys/proc.hpp"

class ThreadPool {

public:
    struct Runnable {
        std::function<void()> function;
        std::promise<void> promise;
    };

private:
    std::vector<std::thread> _threads;
    std::list<Runnable> _job_queue;
    Mutex _job_queue_mutex;
    ConditionVariable _job_queue_cond_var;
    bool _terminate = false;

public:
    ThreadPool(size_t size) : _threads(size) {
        for (size_t i = 0; i < _threads.size(); i++) {
            _threads[i] = std::thread([&, i]() {runThread(i);});
        }
    }
    ~ThreadPool() {
        _terminate = true;
        for (auto& thread : _threads) thread.join();
    }

    std::future<void> addTask(std::function<void()>&& function) {
        Runnable r;
        r.function = std::move(function);
        std::future<void> future;
        {
            auto lock = _job_queue_mutex.getLock();
            _job_queue.push_back(std::move(r));
            future = _job_queue.back().promise.get_future();
        }
        _job_queue_cond_var.notify();
        return future;
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
