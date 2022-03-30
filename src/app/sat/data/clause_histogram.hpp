
#pragma once

#include <vector>
#include <string>

#include "util/sys/atomics.hpp"

class ClauseHistogram {

private:
    std::atomic_ullong _total {0};
    std::vector<std::atomic_ullong*> _hist;

public:
    ClauseHistogram(size_t maxSize): _hist(maxSize) {
        for (size_t i = 0; i < maxSize; i++) _hist[i] = new std::atomic_ullong(0);
    }
    ~ClauseHistogram() {
        for (auto ptr : _hist) delete ptr;
    }

    void increment(size_t size) {
        _hist[std::min(size, _hist.size())-1]->fetch_add(1, std::memory_order_relaxed);
        atomics::incrementRelaxed(_total);
    }

    void increase(size_t size, int by) {
        _hist[std::min(size, _hist.size())-1]->fetch_add(by, std::memory_order_relaxed);
        _total.fetch_add(by, std::memory_order_relaxed);
    }

    std::string getReport() {

        // Find last position where actual information is stored
        size_t endIdx = _hist.size()-1;
        while (_hist[endIdx]->load(std::memory_order_relaxed) == 0 && endIdx > 0) endIdx--;

        // Assemble string
        std::string out = "total:" + std::to_string(_total.load(std::memory_order_relaxed));
        for (size_t i = 0; i <= endIdx; i++) {
            out += " " + std::to_string(_hist[i]->load(std::memory_order_relaxed));
        }
        return out;
    }
};
