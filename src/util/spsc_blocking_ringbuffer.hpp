
#pragma once

#include <vector>
#include "util/sys/atomics.hpp"
#include "util/sys/threading.hpp"
#include "merge_source_interface.hpp"

template <typename T>
class SPSCBlockingRingbuffer : public MergeSourceInterface<T> {

private:
    std::atomic_int _num_elems {0};
    
    std::vector<T> _buffer;
    size_t _buffer_size {0};

    int _read_pos {0};
    int _write_pos {0};

    Mutex _buffer_mutex;
    ConditionVariable _buffer_cond_var;
    
    bool _input_exhausted {false};

public:
    SPSCBlockingRingbuffer() : _buffer(0) {}
    SPSCBlockingRingbuffer(int bufferSize) : _buffer(bufferSize), _buffer_size(bufferSize) {}
    SPSCBlockingRingbuffer(SPSCBlockingRingbuffer&& moved) {
        *this = std::move(moved);
    }
    SPSCBlockingRingbuffer& operator=(SPSCBlockingRingbuffer&& moved) {
        _buffer = std::move(moved._buffer);
        _buffer_size = moved._buffer_size;
        _num_elems = moved._num_elems.load(std::memory_order_relaxed);
        _input_exhausted = moved._input_exhausted;
        return *this;
    }

    void pushBlocking(T& input) {

        int numElems = size();
        if (numElems == _buffer_size) {
            // wait until space is available
            //LOG(V2_INFO, "SPSC wait nonfull\n");
            waitFor([&]() {
                return size() < _buffer_size;
            });
            //LOG(V2_INFO, "SPSC wait nonfull done\n");
        }

        std::swap(_buffer[_write_pos++], input);
        if (_write_pos == _buffer_size) _write_pos = 0;

        numElems = _num_elems.fetch_add(1, std::memory_order_release);
        
        _buffer_mutex.getLock(); // lock buffer and immediately release it, for cond. var.
        _buffer_cond_var.notify();
        //LOG(V2_INFO, "SPSC notify nonempty\n");
    }

    bool pollBlocking(T& out) override {

        int numElems = size();
        if (numElems == 0) {
            // no elements AND input exhausted? => fully exhausted
            if (_input_exhausted) return false;
            
            // wait until elements are there or the input is marked exhausted
            //LOG(V2_INFO, "SPSC wait nonempty or exhausted\n");
            waitFor([&]() {
                return _input_exhausted || !empty();
            });
            //LOG(V2_INFO, "SPSC wait nonempty or exhausted done\n");
            
            // still no elements? => fully exhausted.
            if (empty())
                return false;
        } // else: at least one element is present

        std::swap(_buffer[_read_pos++], out);
        if (_read_pos == _buffer_size) _read_pos = 0;

        numElems = _num_elems.fetch_sub(1, std::memory_order_release);
        
        _buffer_mutex.getLock(); // lock buffer and immediately release it, for cond. var.
        _buffer_cond_var.notify();

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

    size_t getCurrentSize() const override {
        return size();
    }

    int size() const {
        return _num_elems.load(std::memory_order_relaxed);
    }

    bool empty() const {
        return size() == 0;
    }

    bool exhausted() const {
        return _input_exhausted;
    }

private:
    inline void waitFor(std::function<bool()> predicate) {
        //while (!predicate()) usleep(1000);
        _buffer_cond_var.wait(_buffer_mutex, predicate);
    }

};
