
#ifndef DOMPASCH_RANDOM
#define DOMPASCH_RANDOM

#include <random>
#include <functional>

class Random {
public:
    static std::mt19937 _rng;
    static std::uniform_real_distribution<float> _dist;

    static void init(int seed) {
        _rng = std::mt19937(seed);
        _dist = std::uniform_real_distribution<float>(0, 1);
    }

    static float rand() {
        return _dist(_rng);
    }
    static int roundProbabilistically(float x) {
        return rand() < x-(int)x ? std::ceil(x) : std::floor(x);
    }
    static int choice(std::vector<int> vec) {
        return vec[ (int) (vec.size()*rand()) ];
    }
};

#endif
