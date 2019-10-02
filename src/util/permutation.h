
#ifndef DOMPASCH_PERMUTATION
#define DOMPASCH_PERMUTATION

#include <random>
#include <algorithm>
#include <cmath>
#include <unordered_map>

class AdjustablePermutation {

private:
    int n; // size of permutation: [0, n-1]
    int root_n;

    std::mt19937 rng;
    std::vector<std::vector<int>> feistels;

    std::unordered_map<int, int> adjustedValues;

public:
    AdjustablePermutation() = default;
    AdjustablePermutation(int n, int seed);

    int get(int x) const;
    void adjust(int x, int new_x);
    int operator[](int x) const { return get(x); };
    void clear();
};

#endif
