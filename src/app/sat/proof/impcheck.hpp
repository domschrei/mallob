
#pragma once

#include "util/hashing.hpp"

class ImpCheck {
public:
    static unsigned long getKeySeed(int baseSeed) {
        unsigned long keySeed = 1897302748209UL;
        hash_combine(keySeed, 17);
        hash_combine(keySeed, baseSeed);
        hash_combine(keySeed, keySeed);
        return keySeed;
    }
};
