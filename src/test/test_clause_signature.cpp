
#include "app/sat/data/clause.hpp"
#include "util/logger.hpp"
#include "util/random.hpp"
#include "cadical/src/signature.hpp"

#include <unordered_set>
#include <iostream>

#define CLAUSE_SIG_SEED 112589995477

void testSignature() {
    int nbTotal = 100'000'000;
    auto sum {0};
    for (size_t i = 1; i <= nbTotal; i++) {
        int clsLength = 20; //(int) (1 + 300*Random::rand());
        std::vector<int> lits(clsLength, 100*i);
        //for (int k = 0; k < clsLength; k++) {
        //    lits.push_back((int) (-100'000 + 200'000*Random::rand()));
        //}
        uint64_t sig = clause_signature_sign(i, lits.data(), lits.size(), CLAUSE_SIG_SEED);
        sum += sig;
    }
    std::cout << sum << std::endl;
}

int main() {
    testSignature();
}
