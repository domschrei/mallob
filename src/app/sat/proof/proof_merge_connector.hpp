
#pragma once

#include <deque>

#include "comm/distributed_file_merger.hpp"
#include "util/sys/threading.hpp"

class ProofMergeConnector {

private:
    std::atomic_int _num_lines {0};
    
    std::vector<SerializedLratLine> _buffer;
    std::atomic_int _read_pos {0};
    std::atomic_int _write_pos {0};

    Mutex _buffer_mutex;
    ConditionVariable _space_available_cond_var;
    ConditionVariable _line_available_cond_var;
    
    bool _input_exhausted {false};

public:
    ProofMergeConnector() : _buffer(0) {}
    ProofMergeConnector(int bufferSize) : _buffer(bufferSize) {}
    ProofMergeConnector(ProofMergeConnector&& moved) {
        *this = std::move(moved);
    }
    ProofMergeConnector& operator=(ProofMergeConnector&& moved) {
        _buffer = std::move(moved._buffer);
        _num_lines = moved._num_lines.load(std::memory_order_relaxed);
        _input_exhausted = moved._input_exhausted;
        return *this;
    }

    void pushBlocking(SerializedLratLine&& line) {

        int numLines = _num_lines.load(std::memory_order_acquire);
        int writePos = _write_pos.load(std::memory_order_relaxed);

        if (numLines == _buffer.size()) {
            // wait until space is available
            _space_available_cond_var.wait(_buffer_mutex, [&]() {
                return _num_lines.load(std::memory_order_relaxed) < _buffer.size();
            });
        }

        _buffer[writePos].data().swap(line.data());
        _write_pos.store((writePos+1)%_buffer.size(), std::memory_order_relaxed);
        _num_lines.fetch_add(1, std::memory_order_release);

        if (numLines == 0) {
            _buffer_mutex.getLock();
            _line_available_cond_var.notify();
        }
    }

    bool poll(SerializedLratLine& out) {

        int readPos = _read_pos.load(std::memory_order_acquire);
        int numLines = _num_lines.load(std::memory_order_relaxed);

        if (numLines == 0) {
            // wait until a line is available or inputs are exhausted
            if (_input_exhausted) return false;
            _line_available_cond_var.wait(_buffer_mutex, [&]() {
                return _input_exhausted || _num_lines.load(std::memory_order_relaxed) > 0;
            });
        }

        bool bufferEmpty = _num_lines.load(std::memory_order_relaxed) == 0;
        if (bufferEmpty) return false;
        
        _buffer[readPos].data().swap(out.data());
        _read_pos.store((readPos+1)%_buffer.size(), std::memory_order_relaxed);
        _num_lines.fetch_sub(1, std::memory_order_release);

        if (numLines == _buffer.size()) {
            _buffer_mutex.getLock();
            _space_available_cond_var.notify();
        }
        return true;
    }

    void markExhausted() {
        {
            auto lock = _buffer_mutex.getLock();
            _input_exhausted = true;
        }
        _line_available_cond_var.notify();
    }
};
