
#pragma once

#include <vector>
#include "util/logger.hpp"
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
    bool _terminated {false};

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
        _terminated = moved._terminated;
        return *this;
    }

    bool pushBlocking(T& input) {

        if (size() == _buffer_size) {
            // wait until space is available
            //LOG(V2_INFO, "SPSC wait nonfull\n");
            waitFor([&]() {
                return _terminated || size() < _buffer_size;
            });
            if (_terminated) return false;
            //LOG(V2_INFO, "SPSC wait nonfull done\n");
        }

        std::swap(_buffer[_write_pos++], input);
        if (_write_pos == _buffer_size) _write_pos = 0;

        int numElemsBefore = _num_elems.fetch_add(1, std::memory_order_release);
        
        // If the buffer was empty before pushing the element,
        // then it may be possible that a notification is required.
        // If it was NOT empty, then the other thread did not begin waiting.
        if (numElemsBefore == 0) {
            _buffer_mutex.getLock(); // lock buffer and immediately release it, for cond. var.
            _buffer_cond_var.notify();
        }
        //LOG(V2_INFO, "SPSC notify nonempty\n");
        return true;
    }

    bool pollBlocking(T& out) override {
        return pollBlocking(out, false);
    }

    inline bool pollBlocking(T& out, bool returnIfEmpty) {

        if (empty()) {
            if (returnIfEmpty) return false;

            // no elements AND input exhausted? => fully exhausted
            if (_input_exhausted) return false;
            
            // wait until elements are there or the input is marked exhausted
            //LOG(V2_INFO, "SPSC wait nonempty or exhausted\n");
            waitFor([&]() {
                return _input_exhausted || !empty();
            });
            //LOG(V2_INFO, "SPSC wait nonempty or exhausted done\n");
            
            // still no elements? => fully exhausted.
            if (empty()) return false;
        }

        std::swap(_buffer[_read_pos++], out);
        if (_read_pos == _buffer_size) _read_pos = 0;

        int numElemsBefore = _num_elems.fetch_sub(1, std::memory_order_release);
        
        // If the buffer was full before removing the element,
        // then it may be possible that a notification is required.
        // If it was NOT full, then the other thread did not begin waiting.
        if (numElemsBefore == _buffer_size) {
            _buffer_mutex.getLock(); // lock buffer and immediately release it, for cond. var.
            _buffer_cond_var.notify();
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

    void markTerminated() {
        {
            auto lock = _buffer_mutex.getLock();
            _terminated = true;
        }
        _buffer_cond_var.notify();
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
