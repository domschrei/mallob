
#pragma once

#include <vector>

struct Bucket {
    std::vector<int> dataVec;
    int* data; // C style access
    unsigned int size {0};
    int clauseLength {0};
    int lbd {0};
    Bucket(int bucketSize) : dataVec(bucketSize), data(dataVec.data()) {}
};
