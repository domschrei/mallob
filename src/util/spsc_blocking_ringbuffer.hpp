
#pragma once

#include <vector>
#include "util/sys/atomics.hpp"
#include "util/sys/threading.hpp"

template <typename T>
class SPSCBlockingRingbuffer {

private:
    std::atomic_int _num_elems {0};
    
    std::vector<T> _buffer;
    std::atomic_int _read_pos {0};
    std::atomic_int _write_pos {0};

    Mutex _buffer_mutex;
    ConditionVariable _buffer_cond_var;
    
    bool _input_exhausted {false};

public:
    SPSCBlockingRingbuffer() : _buffer(0) {}
    SPSCBlockingRingbuffer(int bufferSize) : _buffer(bufferSize) {}
    SPSCBlockingRingbuffer(SPSCBlockingRingbuffer&& moved) {
        *this = std::move(moved);
    }
    SPSCBlockingRingbuffer& operator=(SPSCBlockingRingbuffer&& moved) {
        _buffer = std::move(moved._buffer);
        _num_elems = moved._num_elems.load(std::memory_order_relaxed);
        _input_exhausted = moved._input_exhausted;
        return *this;
    }

    void pushBlocking(T& input) {

        int numElems = _num_elems.load(std::memory_order_acquire);
        int writePos = _write_pos.load(std::memory_order_relaxed);

        if (numElems == _buffer.size()) {
            // wait until space is available
            //LOG(V2_INFO, "SPSC wait nonfull\n");
            waitFor([&]() {
                return _num_elems.load(std::memory_order_relaxed) < _buffer.size();
            });
            //LOG(V2_INFO, "SPSC wait nonfull done\n");
        }

        std::swap(_buffer[writePos], input);
        _write_pos.store((writePos+1)%_buffer.size(), std::memory_order_relaxed);
        numElems = _num_elems.fetch_add(1, std::memory_order_release);

        if (numElems == 1) {
            _buffer_mutex.getLock();
            _buffer_cond_var.notify();
            //LOG(V2_INFO, "SPSC notify nonempty\n");
        }
    }

    bool poll(T& out) {

        int readPos = _read_pos.load(std::memory_order_acquire);
        int numElems = _num_elems.load(std::memory_order_relaxed);

        if (numElems == 0) {
            // wait until a line is available or inputs are exhausted
            if (_input_exhausted) return false;
            //LOG(V2_INFO, "SPSC wait nonempty or exhausted\n");
            waitFor([&]() {
                return _input_exhausted || _num_elems.load(std::memory_order_relaxed) > 0;
            });
            //LOG(V2_INFO, "SPSC wait nonempty or exhausted done\n");
        }

        bool bufferEmpty = _num_elems.load(std::memory_order_relaxed) == 0;
        if (bufferEmpty) return false;
        
        std::swap(_buffer[readPos], out);
        _read_pos.store((readPos+1)%_buffer.size(), std::memory_order_relaxed);
        numElems = _num_elems.fetch_sub(1, std::memory_order_release);

        if (numElems == _buffer.size()-1) {
            _buffer_mutex.getLock();
            _buffer_cond_var.notify();
            //LOG(V2_INFO, "SPSC notify nonfull\n");
        }
        return true;
    }

    void markExhausted() {
        {
            auto lock = _buffer_mutex.getLock();
            _input_exhausted = true;
        }
        _buffer_cond_var.notify();
        //LOG(V2_INFO, "SPSC notify exhausted\n");
    }

private:
    inline void waitFor(std::function<bool()> predicate) {
        //while (!predicate()) usleep(1000);
        _buffer_cond_var.wait(_buffer_mutex, predicate);
    }

};
