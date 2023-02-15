
#include "random.hpp"

std::mt19937 Random::_rng;
std::mt19937 Random::_global_rng;
std::uniform_real_distribution<float> Random::_dist; 

bool select_next_for_k_from_n(int k, int n, std::function<float()> rngZeroToOne = [](){return Random::rand();}) {
    auto probSelect = 1.0 - (n-k) / (double)n;
    // robustness towards floating-point shenanigans
    if (probSelect > 0 && (probSelect >= 1 || rngZeroToOne() < probSelect)) {
        // array[pos] selected
        return true;
    }
    return false;
}
