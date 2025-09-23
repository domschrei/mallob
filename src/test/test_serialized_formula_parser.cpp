
#include <assert.h>
#include <stdlib.h>
#include <vector>

#include "app/sat/parse/serialized_formula_parser.hpp"
#include "util/logger.hpp"
#include "util/sys/timer.hpp"
#include "util/random.hpp"

void test(const std::vector<int>& payload, int seed) {
    
    SerializedFormulaParser parser(Logger::getMainInstance(), payload.data(), payload.size());
    bool ok;
    int lit;
    for (size_t i = 0; i < payload.size(); i++) {
        ok = parser.getNextLiteral(lit);
        assert(ok);
        LOG(V2_INFO, "%i\n", lit);
    }
    assert(!parser.getNextLiteral(lit));
}

void testLarge() {
    std::vector<int> payload;
    for (size_t i = 0; i < 100'000; i++) {
        payload.push_back(i+1); payload.push_back(0);
    }

    {
        SerializedFormulaParser parser(Logger::getMainInstance(), payload.data(), payload.size());
        bool ok;
        int lit;
        for (size_t i = 0; i < payload.size(); i++) {
            ok = parser.getNextLiteral(lit);
            assert(ok);
            assert(lit == payload[i]);
        }
        assert(!parser.getNextLiteral(lit));
    }
}

int main() {

    Timer::init();
    Random::init(rand(), rand());
    Logger::init(0, V5_DEBG);

    std::vector<int> payload {
        1, 2, 3, 0, 
        4, 5, 6, 0,
        -9, 0,
        -1, -4, -6, 0,
        10, 11, 0
    };
    for (int seed = 1; seed <= 10; seed++) {
        test(payload, seed);
        test(payload, seed);
    }

    testLarge();
}
