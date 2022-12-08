
#ifndef DOMPASCH_MALLOB_SHUFFLE_HPP
#define DOMPASCH_MALLOB_SHUFFLE_HPP

#include <stdlib.h>
#include <random>
#include <functional>

#include "util/random.hpp"

// https://stackoverflow.com/a/6127606
template <typename T>
void shuffle(T* array, size_t n, 
    std::function<float()> rng = [](){return Random::rand();})
{
    if (n <= 1) return; 
    for (size_t i = 0; i < n - 1; i++) {
        size_t j = i + (size_t) (rng() * (n-i));
        std::swap(array[j], array[i]);
    }
}

// https://stackoverflow.com/a/6127606
template <typename T>
void shuffle(T* array, size_t n, SplitMix64Rng& rng)
{
    if (n <= 1) return; 
    for (size_t i = 0; i < n - 1; i++) {
        size_t j = i + (size_t) (rng() % (n-i));
        std::swap(array[j], array[i]);
    }
}

#endif
