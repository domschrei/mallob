
#include "random.h"

std::mt19937 Random::_rng;
std::mt19937 Random::_global_rng;
std::uniform_real_distribution<float> Random::_dist; 
