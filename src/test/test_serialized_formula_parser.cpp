
#include "app/sat/parse/serialized_formula_parser.hpp"
#include "util/logger.hpp"
#include "util/sys/timer.hpp"
#include "util/random.hpp"

void test(const std::vector<int>& payload, int seed, bool shuffleCls, bool shuffleLits) {

    LOG(V2_INFO, "shuffling cls: %s, shuffling literals: %s\n", 
        shuffleCls?"yes":"no", shuffleLits?"yes":"no");
    
    SerializedFormulaParser parser(payload.size(), payload.data());
    parser.shuffle(seed, shuffleCls, shuffleLits);
    bool ok;
    int lit;
    for (size_t i = 0; i < payload.size(); i++) {
        ok = parser.getNextLiteral(lit);
        assert(ok);
        LOG(V2_INFO, "%i\n", lit);
    }
    assert(!parser.getNextLiteral(lit));
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
        test(payload, seed, true, true);
    }
}
