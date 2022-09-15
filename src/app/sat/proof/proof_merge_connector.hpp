
#pragma once

#include <deque>

#include "comm/distributed_file_merger.hpp"
#include "util/sys/threading.hpp"

class ProofMergeConnector {

private:
    const int _buffer_size;

    std::atomic_int _num_lines {0};
    std::deque<SerializedLratLine> _buffer;
    Mutex _buffer_mutex;
    ConditionVariable _space_available_cond_var;
    ConditionVariable _line_available_cond_var;
    bool _input_exhausted {false};

public:
    ProofMergeConnector() : _buffer_size(0) {}
    ProofMergeConnector(int bufferSize) : _buffer_size(bufferSize) {}
    ProofMergeConnector(ProofMergeConnector&& moved) : _buffer_size(moved._buffer_size) {
        *this = std::move(moved);
    }
    ProofMergeConnector& operator=(ProofMergeConnector&& moved) {
        _buffer = std::move(moved._buffer);
        _num_lines = moved._num_lines.load(std::memory_order_relaxed);
        _input_exhausted = moved._input_exhausted;
        return *this;
    }

    void pushBlocking(SerializedLratLine&& line) {
        if (_num_lines.load(std::memory_order_relaxed) == _buffer_size) {
            _space_available_cond_var.wait(_buffer_mutex, [&]() {
                return _num_lines.load(std::memory_order_relaxed) < _buffer_size;
            });
        }
        int newNumLines;
        {
            auto lock = _buffer_mutex.getLock();
            _buffer.push_front(std::move(line));
            newNumLines = _num_lines.fetch_add(1, std::memory_order_relaxed);
        }
        if (newNumLines == 1) _line_available_cond_var.notify();
    }

    bool poll(SerializedLratLine& out) {
        bool bufferEmpty = _num_lines.load(std::memory_order_relaxed) == 0;
        if (bufferEmpty) {
            if (_input_exhausted) return false;
            //LOG(V2_INFO, "poll(out): begin wait\n");
            _line_available_cond_var.wait(_buffer_mutex, [&]() {
                return _input_exhausted || _num_lines.load(std::memory_order_relaxed) > 0;
            });
            //LOG(V2_INFO, "poll(out): end wait\n");
            if (_num_lines.load(std::memory_order_relaxed) == 0) {
                // _input_exhausted is true and the buffer is empty
                //LOG(V2_INFO, "poll(out): fully exhausted\n");
                return false;
            } // else: the buffer is non-empty
        }
        int newNumLines;
        {
            auto lock = _buffer_mutex.getLock();
            assert(!_buffer.empty());
            _buffer.back().data().swap(out.data());
            _buffer.pop_back();
            newNumLines = _num_lines.fetch_sub(1, std::memory_order_relaxed);
        }
        if (newNumLines == _buffer_size - 1) _space_available_cond_var.notify();
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
