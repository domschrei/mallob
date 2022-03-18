
#include "util/hashing.hpp"
#include "app/sat/hordesat/utilities/clause.hpp"

#include <unordered_set>
#include <iostream>

int main() {

    std::unordered_set<size_t> goodHashes;
    std::unordered_set<size_t> observedHashes;
    for (int lit = -1000000; lit <= 1000000; lit++) {
        if (lit == 0) continue;

        observedHashes.insert(Mallob::nonCommutativeHash(&lit, 1, 3) << 40);
        goodHashes.insert(robin_hood::hash<int>()(lit) << 40);
    }

    std::cout << observedHashes.size() << " / 2'000'000 unique hashes observed" << std::endl;
    std::cout << goodHashes.size() << " / 2'000'000 unique \"good\" hashes" << std::endl;
}
