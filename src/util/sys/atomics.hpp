
#ifndef DOMPASCH_MALLOB_ATOMICS_HPP
#define DOMPASCH_MALLOB_ATOMICS_HPP

#include <atomic>

namespace atomics {
    
    template <typename T>
    void incrementRelaxed(std::atomic<T>& var) {
        var.fetch_add(1, std::memory_order_relaxed);
    }

    template <typename T>
    void decrementRelaxed(std::atomic<T>& var) {
        var.fetch_sub(1, std::memory_order_relaxed);
    }

    template <typename T>
    void addRelaxed(std::atomic<T>& var, T by) {
        var.fetch_add(by, std::memory_order_relaxed);
    }

    template <typename T>
    void subRelaxed(std::atomic<T>& var, T by) {
        var.fetch_sub(by, std::memory_order_relaxed);
    }
};

#endif
