
#pragma once

#include "stxxl/priority_queue"
#include <limits>

template <typename T>
struct ComparatorLess {
    bool operator()(const T&a, const T&b) const { 
        return a < b; 
    }
    int min_value() const { 
        return std::numeric_limits<T>::min(); 
    }
};
    
// Allowed data in main memory.
#define EXTPQUEUE_INTERNAL_MEMORY_BYTES (128*1024*1024) // 128 MiB

// Max. number of elements overall.
// allow for X GB of data in external memory:
// 1024*n * sizeof(T) = X GiB
// n = X GiB / (1024 * sizeof(T)) = X MiB / sizeof(T)
#define EXTPQUEUE_MAX_EXTERNAL_ELEMENTS_1024S ((32*1024*1024)/sizeof(T)) // leads to 32GiB


template <typename T>
class ExternalPriorityQueue {
    
private:
    typedef typename stxxl::PRIORITY_QUEUE_GENERATOR<T, ComparatorLess<T>, EXTPQUEUE_INTERNAL_MEMORY_BYTES, EXTPQUEUE_MAX_EXTERNAL_ELEMENTS_1024S>::result pqueue_type;
    typedef typename pqueue_type::block_type block_type;

    stxxl::read_write_pool<block_type> _pool;
    pqueue_type _queue;

public:
    ExternalPriorityQueue() : _pool(32, 32), _queue(_pool) {}
    
    void push(const T& elem) {
        _queue.push(elem);
    }

    const T& top() const {
        return _queue.top();
    }

    T pop() {
        T elem = _queue.top();
        _queue.pop();
        return elem;
    }

    unsigned long long capacity() const {
        return EXTPQUEUE_MAX_EXTERNAL_ELEMENTS_1024S * 1024ULL;
    }

    bool empty() const {
        return _queue.empty();
    }

    size_t size() const {
        return _queue.size();
    }
};
