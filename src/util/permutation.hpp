
#ifndef DOMPASCH_PERMUTATION
#define DOMPASCH_PERMUTATION

#include <random>
#include <algorithm>
#include <cmath>
#include "util/hashing.hpp"

class AdjustablePermutation {

private:
    int _n; // size of permutation: [0, n-1]
    int _root_n;

    std::mt19937 _rng;
    std::vector<std::vector<int>> _feistels;

    robin_hood::unordered_map<int, int> _adjusted_values;

    bool _identity_disallowed = false;
    std::vector<AdjustablePermutation*> _disallowed_permutations;

public:
    static std::vector<int> createUndirectedExpanderGraph(int n, int r, int myRank);
    
    static std::vector<std::vector<int>> getPermutations(int n, int r);
    static std::vector<int> createExpanderGraph(const std::vector<std::vector<int>>& permutations, int myRank);
    static std::vector<int> getBestOutgoingEdgeForEachNode(const std::vector<std::vector<int>>& permutations, int myRank);

    AdjustablePermutation() = default;
    AdjustablePermutation(int n, int seed);

    int get(int x, bool checkAdjusted = true) const;
    void adjust(int x, int new_x);
    void clear(int x);
    int operator[](int x) const { return get(x); };
    void clear();

    void setIdentityDisallowed(const bool& disallow);
    void addDisallowedPermutation(AdjustablePermutation* p);
};

#endif
