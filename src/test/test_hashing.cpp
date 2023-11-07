
#include "util/hashing.hpp"
#include "app/sat/data/clause.hpp"
#include "util/logger.hpp"
#include "util/random.hpp"

#include <unordered_set>
#include <iostream>

size_t commutativeHash(const int* begin, int size, int which = 3) {
    static unsigned const int primes [] = 
        {2038072819, 2038073287, 2038073761, 2038074317,
        2038072823,	2038073321,	2038073767, 2038074319,
        2038072847,	2038073341,	2038073789,	2038074329,
        2038074751,	2038075231,	2038075751,	2038076267};
    
    size_t res = 1;
    for (auto it = begin; it != begin+size; it++) {
        int lit = *it;
        res ^= lit * primes[abs((lit^which) & 15)];
    }
    return res;
}

size_t nonCommutativeHash(const int* begin, int size, int which = 3) {
    size_t res = robin_hood::hash_int(size * which);
    for (size_t i = 0; i < size; i++) {
        hash_combine(res, begin[i]);
    }
    return res;
}

void testCollisions() {
    for (int shift = 0; shift < 64; shift += 4) {
        std::cout << "shift=" << shift << std::endl;

        //std::unordered_set<size_t> commHashes;
        std::unordered_set<size_t> noncommHashes;
        std::unordered_set<size_t> rhHashes;
        for (int lit = -1000000; lit <= 1000000; lit++) {
            if (lit == 0) continue;

            //commHashes.insert(commutativeHash(&lit, 1, 3) >> shift);
            noncommHashes.insert(nonCommutativeHash(&lit, 1, 3) << shift);
            rhHashes.insert(robin_hood::hash<int>()(lit) << shift);
        }

        //std::cout << commHashes.size() << " / 2'000'000 unique commutative hashes" << std::endl;
        std::cout << noncommHashes.size() << " / 2'000'000 unique non-commutative hashes" << std::endl;
        std::cout << rhHashes.size() << " / 2'000'000 unique robin_hood hashes" << std::endl;
    }
}

void testNonCommutativeHashFunctionDistribution() {
    auto hasher = Mallob::NonCommutativeClauseHasher();

    int nbBuckets = 256;
    std::vector<int> buckets(nbBuckets);
    int nbTotal = 100'000;
    for (size_t i = 0; i < nbTotal; i++) {
        int clsLength = (int) (1 + 300*Random::rand());
        std::vector<int> lits;
        for (int k = 0; k < clsLength; k++) {
            lits.push_back((int) (-100'000 + 200'000*Random::rand()));
        }
        Mallob::Clause c(lits.data(), clsLength, 2);
        auto h = hasher(c);
        buckets[h % nbBuckets]++;
    }

    for (int b = 0; b < nbBuckets; b++) {
        LOG(V2_INFO, "b=%i\tn=%i\tr=%.3f\n", b, buckets[b], buckets[b]/(float)nbTotal);
    }
}

int main() {
    testCollisions();
    testNonCommutativeHashFunctionDistribution();
}
