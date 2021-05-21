
#ifndef DOMPASCH_MALLOB_CHECKSUM_HPP
#define DOMPASCH_MALLOB_CHECKSUM_HPP

#include "util/hashing.hpp"

class Checksum {

private:
    size_t _count = 0;
    size_t _val = 0;

public:
    void combine(int x) {
        _count++;
        hash_combine(_val, x);
    }
    size_t get() const {return _val;}
    size_t count() const {return _count;}
};

#endif