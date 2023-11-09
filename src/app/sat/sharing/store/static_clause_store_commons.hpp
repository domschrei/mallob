
#pragma once

#include <cstddef>
#include <vector>

struct Bucket {
    std::vector<int> dataVec;
    int* data; // C style access
    unsigned int size {0};
    int clauseLength {0};
    int lbd {0};
    Bucket(int bucketSize) : dataVec(bucketSize), data(dataVec.data()) {}

    const size_t capacity() const {return dataVec.size();}
    void expand() {
        dataVec.resize(dataVec.empty() ? 256 : dataVec.size() * 2);
        data = dataVec.data();
    }
    void shrinkToFit() {
        dataVec.resize(size);
        data = dataVec.data();
    }
};
