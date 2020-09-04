
#ifndef DOMPASCH_RANDOM
#define DOMPASCH_RANDOM

#include <random>
#include <functional>
#include <set>
#include <vector>
#include <assert.h>

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
    static int choice(std::vector<int> vec) {
        return vec[ (int) (vec.size()*rand()) ];
    }
    static int choice(std::set<int> set) {
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

#endif
