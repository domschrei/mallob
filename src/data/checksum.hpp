
#ifndef DOMPASCH_MALLOB_CHECKSUM_HPP
#define DOMPASCH_MALLOB_CHECKSUM_HPP

#include "util/hashing.hpp"

class Checksum {

private:
    size_t _count = 0;
    size_t _val = 0;

public:
    Checksum() {}
    Checksum(size_t count, size_t val) : _count(count), _val(val) {}
    void combine(int x) {
        _count++;
        hash_combine(_val, x);
    }
    size_t get() const {return _val;}
    size_t count() const {return _count;}
    bool operator==(const Checksum& other) const {
        return _count == other._count && _val == other._val;
    }
    bool operator!=(const Checksum& other) const {
        return !(*this == other);
    }
};

#endif