
#pragma once

#include <queue>

#include "util/categorized_external_memory.hpp"

class ExternalIdPriorityQueue {

private:
    int _current_epoch;
    CategorizedExternalMemory<unsigned long> _ext_mem;
    std::priority_queue<unsigned long> _internal_queue;

public:
    ExternalIdPriorityQueue(const std::string& diskFilename, int maxEpoch) : 
        _current_epoch(maxEpoch), _ext_mem(diskFilename, 4096) {}

    void push(unsigned long id, int epoch) {
        if (epoch >= _current_epoch) {
            _internal_queue.push(id);
        } else {
            _ext_mem.add(epoch, id);
        }
    }

    unsigned long top() {
        if (_internal_queue.empty()) {
            refill();
        }
        assert(!_internal_queue.empty());
        return _internal_queue.top();
    }

    unsigned long pop() {
        auto id = top();
        _internal_queue.pop();
        return id;
    }

    unsigned long long size() const {
        return _internal_queue.size() + _ext_mem.size();
    }
    bool empty() const {
        return size() == 0;
    }

private:

    void refill() {
        assert(_internal_queue.empty());
        std::vector<unsigned long> result;
        while (result.empty()) {
            if (_current_epoch < 0) {
                assert(empty() || log_return_false("%i elements remaining in ext. memory!\n", _ext_mem.size()));
                return;
            }
            _ext_mem.fetchAndRemove(_current_epoch, result);
            if (result.empty()) _current_epoch--;
        }
        for (auto id : result) {
            _internal_queue.push(id);
        }
    }
};
