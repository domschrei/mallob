
#ifndef DOMPASCH_RANDOM
#define DOMPASCH_RANDOM

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <random>
#include <functional>
#include <set>
#include <vector>
#include <cmath>
#include <limits>

#include "util/assert.hpp"

class Random {
public:
    static std::mt19937 _rng;
    static std::mt19937 _global_rng;
    static std::uniform_real_distribution<float> _dist;

    static void init(int globalSeed, int localSeed) {
        _global_rng = std::mt19937(globalSeed);
        _rng = std::mt19937(localSeed);
        _dist = std::uniform_real_distribution<float>(0, 1);
    }

    /*
    Draw a random float in [0,1) from the RNG that is seeded
    GLOBALLY, i.e., the i-th call of this method
    will return the same value on no matter which node.
    */
    static float global_rand() {
        return _dist(_global_rng);
    }

    /*
    Draw a random float in [0,1) from the locally seeded RNG.
    */
    static float rand() {
        return _dist(_rng);
    }
    static int roundProbabilistically(float x) {
        return rand() < x-(int)x ? std::ceil(x) : std::floor(x);
    }
    template<typename T>
    static T& choice(T* data, size_t size) {
        return data[ (size_t) (size*rand()) ];
    }
    template<typename T>
    static const T& choice(const T* data, size_t size) {
        return data[ (size_t) (size*rand()) ];
    }
    template<typename T>
    static T& choice(std::vector<T>& vec) {
        return vec[ (size_t) (vec.size()*rand()) ];
    }
    template<typename T>
    static const T& choice(const std::vector<T>& vec) {
        return vec[ (size_t) (vec.size()*rand()) ];
    }
    static int choice(const std::set<int>& set) {
        assert(!set.empty());
        size_t picked = rand() * set.size();
        assert(picked < set.size());
        size_t i = 0;
        for (const int& entry : set) {
            if (i == picked) return entry;
            i++;
        }
        abort();
    }
};

class SplitMix64Rng {

private:
    uint64_t _state {0};

public:
    SplitMix64Rng() = default;
    SplitMix64Rng(uint64_t seed) : _state(seed) {}

    uint64_t operator()() {
        uint64_t z = (_state += UINT64_C(0x9E3779B97F4A7C15));
        z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
        z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
        return z ^ (z >> 31);
    }

    double randomInRange(double low, double high) {
        static constexpr double maxNum = (double) std::numeric_limits<uint64_t>::max();
        assert(low < high);
        double result = high;
        while (result >= high) {
            uint64_t r = (*this)();
            result = low + (high - low) * (r / maxNum);
        }
        assert(result >= low);
        assert(result < high);
        return result;
    }

    uint64_t max() const {
        return std::numeric_limits<uint64_t>::max();
    }
};

// https://stackoverflow.com/a/6127606
template <typename T>
void random_shuffle(T* array, size_t n, 
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
void random_shuffle(T* array, size_t n, SplitMix64Rng& rng)
{
    if (n <= 1) return; 
    for (size_t i = 0; i < n - 1; i++) {
        size_t j = i + (size_t) (rng() % (n-i));
        std::swap(array[j], array[i]);
    }
}

// Given that you want to choose k elements i.i.d. from n elements,
// decides randomly whether the first element should be selected.
bool select_next_for_k_from_n(int k, int n, std::function<float()> rngZeroToOne);
bool select_next_for_k_from_n(int k, int n, SplitMix64Rng& rng);

/*
choose k elements i.i.d. from sequence of n elements A[0..n), in a single linear pass

P(A[0] picked)  = 1 - P(A[0] not picked)
                = 1 - P(elem. not picked k times from urn with n elements without returning)
                = 1 - ( (n-1)/n * (n-2)/(n-1) * ... * (n-k)/(n-k+1) )
                = 1 - (n-k)/n       // all other terms are cancelled

=> Pick first element A[0] with prob. 1 - (n-k)/n
=> Picked: Continue with A[1] for k'=k-1, n'=n-1
=> Not picked: Continue with A[1] for k'=k, n'=n-1
*/
template <typename T>
std::vector<T> random_choice_k_from_n(const T* array, size_t arraySize, int k, 
        std::function<float()> rngZeroToOne = [](){return Random::rand();}) {

    int numRemainingElems = arraySize;
    int numToSelect = k;
    if (numToSelect >= numRemainingElems) return std::vector<T>(array, array+arraySize);

    std::vector<T> selectedElems;

    size_t pos = 0;
    while (numToSelect > 0) {
        assert(numRemainingElems >= numToSelect);
        // select array[pos] or not?
        auto probSelect = 1 - (numRemainingElems-numToSelect) / (double)numRemainingElems;
        // robustness towards floating-point shenanigans
        if (probSelect > 0 && (probSelect >= 1 || rngZeroToOne() < probSelect)) {
            // array[pos] selected
            numToSelect--;
            selectedElems.push_back(array[pos]);
        }
        numRemainingElems--;
        pos++;
    }

    return selectedElems;
}

#endif
