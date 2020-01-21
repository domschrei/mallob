
#ifndef DOMPASCH_PERMUTATION
#define DOMPASCH_PERMUTATION

#include <random>
#include <algorithm>
#include <cmath>
#include <unordered_map>

class AdjustablePermutation {

private:
    int _n; // size of permutation: [0, n-1]
    int _root_n;

    std::mt19937 _rng;
    std::vector<std::vector<int>> _feistels;

    std::unordered_map<int, int> _adjusted_values;

public:
    AdjustablePermutation() = default;
    AdjustablePermutation(int n, int seed);

    int get(int x) const;
    void adjust(int x, int new_x);
    int operator[](int x) const { return get(x); };
    void clear();
};

#endif
