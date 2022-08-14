
#pragma once

#include <vector>
#include "util/hashing.hpp"

template <typename T>
class BloomFilter {

private:
    std::vector<bool> _bitset;
    int _num_functions;

    std::vector<size_t> _stored_positions;

public:
    BloomFilter(unsigned long size, int numFunctions) : _bitset(size, false), _num_functions(numFunctions) {
        _stored_positions.resize(_num_functions);
    }
    bool tryInsert(const T& elem) {
        static robin_hood::hash<T> h;
        size_t baseHash = h(elem);
        bool canBeInserted = false;
        for (size_t i = 0; i < _num_functions; i++) {
            size_t hash = baseHash;
            hash_combine(hash, 17*i);
            _stored_positions[i] = hash % _bitset.size();
            if (!_bitset[_stored_positions[i]]) canBeInserted = true;
        }
        if (canBeInserted) {
            for (size_t pos : _stored_positions) {
                _bitset[pos] = true;
            }
        }
        return canBeInserted;
    }

};
