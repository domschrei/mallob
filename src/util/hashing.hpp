
#ifndef DOMPASCH_MALLOB_HASHING_HPP
#define DOMPASCH_MALLOB_HASHING_HPP

#include "util/robin_hood.hpp"

template <class T>
inline void hash_combine(std::size_t & s, const T & v)
{
  static robin_hood::hash<T> h;
  s ^= h(v) + 0x9e3779b9 + (s<< 6) + (s>> 2);
}

struct IntPairHasher {
    size_t operator()(const std::pair<int, int>& pair) const {
        size_t h = 17;
        hash_combine(h, pair.first);
        hash_combine(h, pair.second);
        return h;
    }
};

#endif