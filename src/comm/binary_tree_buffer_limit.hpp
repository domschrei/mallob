
#pragma once

#include <cmath>

struct BinaryTreeBufferLimit {
    enum BufferQueryMode {LEVEL=0, LIMITED=1};
    static size_t getLimit(int numWorkers, int baseSize, float functionParam, BufferQueryMode mode) {
        if (mode == LEVEL) {
            float limit = baseSize * std::pow(functionParam, std::log2(numWorkers+1)-1);
            return std::ceil(numWorkers * limit);
        }
        if (mode == LIMITED) {
            float upperBound = functionParam;
            auto buflim = upperBound - (upperBound - baseSize) * std::exp((baseSize / (baseSize - upperBound)) * (numWorkers-1));
            return std::ceil(buflim);
        }
        return 0;
    }
};
