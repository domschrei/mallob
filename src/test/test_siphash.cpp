
#include "app/sat/data/clause.hpp"
#include "util/logger.hpp"
#include "util/random.hpp"
#include "util/SipHash/siphash.hpp"
#include "util/SipHash/siphash.h"
#include "util/sys/timer.hpp"

#include <cstdlib>
#include <unordered_set>
#include <iostream>
#include <vector>

uint8_t key[] = {1, 2, 3, 4, 5, 6, 7, 8,
    45, 24, 76, 3, 2, 5, 9, 99};
SipHash* hasher;

void testSipHash(int inputSize, int blockSize) {

    hasher->reset();

    std::vector<uint8_t> someData = {1, 2, 3, 4, 5, 6, 7, 8, 3, 56, 4, 23, 45, 66, 78, 5, 6, 45, 3, 4, 34, 23, 45, 56, 67};
    someData.resize(inputSize);

    uint8_t* outDirect = (uint8_t*) malloc(128 / 8);
    siphash(someData.data(), someData.size(), key, outDirect, 128/8);

    for (int pos = 0; pos < someData.size(); pos += blockSize) {
        hasher->update(someData.data()+pos, std::min((size_t)blockSize, someData.size()-pos));
    }
    uint8_t* outSteps = hasher->digest();

    for (int i = 0; i < 16; i++) assert(outDirect[i] == outSteps[i] || log_return_false("ERROR at block size %i", blockSize));
}

int main() {
    Timer::init();
    Random::init(rand(), rand());
    Logger::init(0, V5_DEBG);

    hasher = new SipHash(key);

    for (int inputSize = 1; inputSize <= 100; inputSize++) {
        for (int blockSize = 1; blockSize < 25; blockSize++) {
            testSipHash(inputSize, blockSize);
        }
    }
}
