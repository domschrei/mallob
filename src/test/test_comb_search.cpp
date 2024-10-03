
#include <cstdlib>
#include <set>
#include <stdlib.h>
#include <bitset>
#include <vector>

#include "util/sys/timer.hpp"
#include "util/logger.hpp"
#include "util/sys/proc.hpp"
#include "util/random.hpp"
#include "util/sys/process.hpp"
#include "app/maxsat/comb_search.hpp"

void test() {
    CombSearch s(0.9);
    s.init(0, 10'000);
    size_t bound {-1UL};
    bool ok;

    for (int x = 0; x < 10; x++) {
        ok = s.getNextBound(bound); assert(ok); LOG(V2_INFO, "Bound %lu\n", bound);
    }

    s.stopTestingAndUpdateUpper(10'000, 9999);
    ok = s.getNextBound(bound); assert(ok); LOG(V2_INFO, "Bound %lu\n", bound);
    s.stopTestingAndUpdateLower(7290);
    ok = s.getNextBound(bound); assert(ok); LOG(V2_INFO, "Bound %lu\n", bound);
    s.stopTestingAndUpdateUpper(9999, 9991);
    s.stopTestingAndUpdateUpper(9990, 9899);
    s.stopTestingAndUpdateUpper(9900, 9810);
    s.stopTestingAndUpdateUpper(9810, 9800);
    s.stopTestingAndUpdateUpper(8829, 8100);
    ok = s.getNextBound(bound); assert(ok); LOG(V2_INFO, "Bound %lu\n", bound);
    ok = s.getNextBound(bound); assert(ok); LOG(V2_INFO, "Bound %lu\n", bound);
    ok = s.getNextBound(bound); assert(ok); LOG(V2_INFO, "Bound %lu\n", bound);
    ok = s.getNextBound(bound); assert(ok); LOG(V2_INFO, "Bound %lu\n", bound);
    ok = s.getNextBound(bound); assert(ok); LOG(V2_INFO, "Bound %lu\n", bound);
}

void testExhaustive() {
    CombSearch s(0.9);
    s.init(0, 1'000'000);
    size_t bound {-1UL};
    bool ok;

    for (int x = 0; x < 10; x++) {
        ok = s.getNextBound(bound);
        assert(ok);
    }

    while (ok) {
        auto bounds = s.getActiveSearches();
        int r = (int) (bounds.size()*Random::rand());
        int i = 0;
        for (auto it = bounds.begin(); it != bounds.end(); ++it) {
            if (i == r) {
                if (r < bounds.size()/2) {
                    s.stopTestingAndUpdateLower(*it);
                } else {
                    s.stopTestingAndUpdateUpper(*it, 0.99 * *it);
                }
                break;
            }
            i++;
        }
        while (ok && s.getActiveSearches().size() < 10) {
            ok = s.getNextBound(bound);
        }
    }
}

int main() {
    Timer::init();
    Random::init(rand(), rand());
    Logger::init(0, V5_DEBG);
    Process::init(0);

    for (int i=1; i<100; i++) testExhaustive();
}
