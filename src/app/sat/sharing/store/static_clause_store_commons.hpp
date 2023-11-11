
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
        dataVec.resize(dataVec.size() <= 256 ? 512 : dataVec.size() * 2);
        data = dataVec.data();
    }
    bool shrinkable() {
        if (size == 0) return dataVec.capacity() > 0;
        if (size < 427) return false;
        return dataVec.capacity() >= 2*size;
    }
    void shrinkToFit(int s) {
        if (s < size) return;
        if (s == 0) dataVec.clear();
        else dataVec.resize(1.2*s);
        data = dataVec.data();
    }
};
