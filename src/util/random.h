
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
        float random = rand();
        float remainder = x - (int) x;
        if (random < remainder) {
            return std::ceil(x);
        } else {
            return std::floor(x);
        }
    }
    static int choice(std::vector<int> vec) {
        return vec[ (int) (vec.size()*rand()) ];
    }
};

#endif
