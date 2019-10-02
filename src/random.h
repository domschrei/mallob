
#ifndef DOMPASCH_RANDOM
#define DOMPASCH_RANDOM

#include <random>
#include <functional>

class Random {
public:
    static std::mt19937 rng;
    static std::uniform_real_distribution<float> dist;

    static void init(int seed) {
        rng = std::mt19937(seed);
        dist = std::uniform_real_distribution<float>(0, 1);
    }

    static float rand() {
        return dist(rng);
    }
};

#endif
