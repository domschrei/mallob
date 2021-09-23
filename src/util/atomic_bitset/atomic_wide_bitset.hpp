
#ifndef DOMPASCH_ATOMIC_WIDE_BITSET_HPP
#define DOMPASCH_ATOMIC_WIDE_BITSET_HPP

#include <array>

#include "atom_wrapper.hpp"

class AtomicWideBitset {

private:
    std::vector<atomwrapper<char>> _data;

public:
    AtomicWideBitset(int size) : _data(size, 0) {}

    bool test(int idx) {
        return _data[idx].ref().load(std::memory_order_relaxed);
    }
    void set(int idx) {
        set(idx, 1);
    }
    void reset(int idx) {
        set(idx, 0);
    }
    void set(int idx, bool value) {
        _data[idx].ref().store(value ? 1:0, std::memory_order_relaxed);
    }
    void reset() {
        for (size_t i = 0; i < _data.size(); i++) reset(i);
    }
};

#endif
