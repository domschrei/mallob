
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
#include "app/maxsat/interval_search.hpp"

void test() {
    IntervalSearch s(0.9);
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

void testIllustrativeExample() {
    IntervalSearch s(0.5);
    s.init(0, 199);
    size_t bound {-1UL};
    bool ok;

    ok = s.getNextBound(bound); assert(ok); assert(bound == 199 || log_return_false("ERROR: unexpected bound %lu\n", bound));
    ok = s.getNextBound(bound); assert(ok); assert(bound == 99 || log_return_false("ERROR: unexpected bound %lu\n", bound));
    ok = s.getNextBound(bound); assert(ok); assert(bound == 149 || log_return_false("ERROR: unexpected bound %lu\n", bound));

    s.stopTestingAndUpdateUpper(149, 120);

    ok = s.getNextBound(bound); assert(ok); assert(bound == 119 || log_return_false("ERROR: unexpected bound %lu\n", bound));
    ok = s.getNextBound(bound); assert(ok); assert(bound == 49 || log_return_false("ERROR: unexpected bound %lu\n", bound));

    s.stopTestingAndUpdateLower(99);

    ok = s.getNextBound(bound); assert(ok); assert(bound == 109 || log_return_false("ERROR: unexpected bound %lu\n", bound));
    ok = s.getNextBound(bound); assert(ok); assert(bound == 114 || log_return_false("ERROR: unexpected bound %lu\n", bound));
}

void testIllustrativeExampleScaled() {
    IntervalSearch s(0.5);
    s.init(0, 31);
    size_t bound {-1UL};
    bool ok;

    ok = s.getNextBound(bound); assert(ok); assert(bound == 31 || log_return_false("ERROR: unexpected bound %lu\n", bound));
    LOG(V2_INFO, "S1: %lu\n", bound);
    ok = s.getNextBound(bound); assert(ok); assert(bound == 15 || log_return_false("ERROR: unexpected bound %lu\n", bound));
    LOG(V2_INFO, "S2: %lu\n", bound);
    ok = s.getNextBound(bound); assert(ok); assert(bound == 23 || log_return_false("ERROR: unexpected bound %lu\n", bound));
    LOG(V2_INFO, "S3: %lu\n", bound);

    // S3 --> stops S1
    s.stopTestingAndUpdateUpper(23, 20);

    ok = s.getNextBound(bound); assert(ok); assert(bound == 19 || log_return_false("ERROR: unexpected bound %lu\n", bound));
    LOG(V2_INFO, "S3: %lu\n", bound);
    ok = s.getNextBound(bound); assert(ok); assert(bound == 7 || log_return_false("ERROR: unexpected bound %lu\n", bound));
    LOG(V2_INFO, "S1: %lu\n", bound);

    // S2 --> stops S1
    s.stopTestingAndUpdateLower(15);

    ok = s.getNextBound(bound); assert(ok); assert(bound == 17 || log_return_false("ERROR: unexpected bound %lu\n", bound));
    LOG(V2_INFO, "S2: %lu\n", bound);
    ok = s.getNextBound(bound); assert(ok); assert(bound == 18 || log_return_false("ERROR: unexpected bound %lu\n", bound));
    LOG(V2_INFO, "S1: %lu\n", bound);
}

void testExhaustive() {
    IntervalSearch s(0.9);
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

    testIllustrativeExampleScaled();
    //for (int i=1; i<100; i++) testExhaustive();
}
