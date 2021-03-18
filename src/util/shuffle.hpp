
#ifndef DOMPASCH_MALLOB_SHUFFLE_HPP
#define DOMPASCH_MALLOB_SHUFFLE_HPP

#include <stdlib.h>

#include "util/random.hpp"

// https://stackoverflow.com/a/6127606
void shuffle(int* array, size_t n)
{
    if (n <= 1) return; 
    for (size_t i = 0; i < n - 1; i++) {
        size_t j = i + (int) (Random::rand() * (n-i));
        int t = array[j];
        array[j] = array[i];
        array[i] = t;
    }
}

#endif
