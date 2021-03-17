
#ifndef DOMPASCH_MALLOB_CONCURRENT_ALLOCATOR_HPP
#define DOMPASCH_MALLOB_CONCURRENT_ALLOCATOR_HPP

#include <atomic>
#include <list>
#include <thread>
#include <vector>

#include "util/logger.hpp"
#include "util/sys/threading.hpp"

template <typename T>
class ConcurrentAllocator {

private:
    std::atomic_bool _exiting = false;

    std::thread _thread;
    std::atomic_bool _thread_done = true;

    std::atomic_int _num_open = 0;
    Mutex _work_mutex;
    std::list<std::pair<T, size_t>> _work_queue;
    
    std::atomic_int _num_ready = 0;
    Mutex _ready_mutex;
    std::list<std::pair<T, std::shared_ptr<std::vector<uint8_t>>>> _ready_list;

public:
    void order(T identifier, size_t size) {
        
        // Enqueue work to be done
        {
            auto lock = _work_mutex.getLock();
            _work_queue.emplace_back(identifier, size);
        }
        _num_open++;

        if (_thread_done) startThread();
    }

    std::shared_ptr<std::vector<uint8_t>> poll(T& identifier) {

        if (_thread_done && _num_open > 0) startThread();
        if (_num_ready == 0) return std::shared_ptr<std::vector<uint8_t>>();
        auto lock = _ready_mutex.getLock();
        auto [id, ptr] = _ready_list.front();
        _ready_list.pop_front();
        identifier = std::move(id);
        _num_ready--;
        return ptr;
    }

    void startThread() {
        // Join old thread
        if (_thread.joinable()) _thread.join();
        _thread_done = false;
        // Spawn new thread
        _thread = std::thread([this]() {
            while (!_exiting && _num_open > 0) {
                
                _work_mutex.lock();
                auto [id, s] = _work_queue.front();
                _work_queue.pop_front();
                _work_mutex.unlock();
                
                auto ptr = std::shared_ptr<std::vector<uint8_t>>(new std::vector<uint8_t>(s));

                {
                    auto lock = _ready_mutex.getLock();
                    _ready_list.emplace_back(id, ptr);
                    _num_ready++;
                }
                _num_open--;
            }
            _thread_done = true;
        });
    }

    ~ConcurrentAllocator() {
        _exiting = true;
        if (_thread.joinable()) _thread.join();
    }
};

#endif
