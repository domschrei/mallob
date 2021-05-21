
#include <iostream>
#include <assert.h>
#include <vector>
#include <string>
#include <thread>
#include <set>

#include "util/random.hpp"
#include "util/sat_reader.hpp"
#include "util/logger.hpp"
#include "util/sys/timer.hpp"
#include "util/ringbuffer.hpp"

void testMixedRingbuffer() {
    log(V2_INFO, "Testing mixed non-unit ringbuffer ...\n");

    MixedNonunitClauseRingBuffer r(20, 3);

    std::thread producer1([&r]() {
        int v[] = {1, 2, -3}; Clause c{v, 3, /*lbd=*/2};
        int numInserted = 0;
        while (numInserted < 300) {
            if (r.insertClause(c, 0)) numInserted++;
        }
        log(V2_INFO, "Prod. 1: Done!\n");
    });
    std::thread producer2([&r]() {
        int v[] = {-4, -5}; Clause c{v, 2, /*lbd=*/2};
        int numInserted = 0;
        while (numInserted < 300) {
            if (r.insertClause(c, 1)) numInserted++;
        }
        log(V2_INFO, "Prod. 2: Done!\n");
    });
    std::thread producer3([&r]() {
        int v[] = {-6, 7, -8, 9}; Clause c{v, 4, /*lbd=*/4};
        int numInserted = 0;
        while (numInserted < 300) {
            if (r.insertClause(c, 2)) numInserted++;
        }
        log(V2_INFO, "Prod. 3: Done!\n");
    });

    // Consume clauses
    std::vector<int> clauses;
    int expectedSize = 300*5+300*4+300*6;
    while (clauses.size() < expectedSize) {
        bool success = r.getClause(clauses);
        if (success) log(V3_VERB, "Read %i lits\n", clauses.size());
    }
    assert(clauses.size() == expectedSize);

    producer1.join();
    producer2.join();
    producer3.join();

    for (int lit : clauses) log(LOG_NO_PREFIX | V4_VVER, "%i ", lit);
    log(LOG_NO_PREFIX | V4_VVER, "\n");
}

void testUnitBuffer() {
    log(V2_INFO, "Testing unit ringbuffer ...\n");
    UnitClauseRingBuffer r(20, 4);

    std::vector<std::thread> threads;
    for (size_t i = 0; i < 4; i++) {
        threads.emplace_back([i, &r]() {
            for (size_t j = i*1000; j < i*1000+1000; j++) {
                while (!r.insertUnit(j+1, i));
            }
        });
    }

    std::set<int> numbers;
    while (numbers.size() < 4000) {
        std::vector<int> buf;
        bool success = r.getUnits(buf);
        if (success) {
            numbers.insert(buf.begin(), buf.end());
            log(V3_VERB, "Got %i units\n", numbers.size());
        }
    }

    std::vector<int> buf;
    assert(!r.getUnits(buf));

    for (auto& thread : threads) thread.join();
}

void testUniformClauseBuffer() {
    log(V2_INFO, "Testing uniform clause ringbuffer ...\n");

    int clauseSize = 10;
    int numThreads = 100;
    UniformClauseRingBuffer r(1000, clauseSize, numThreads);
    std::vector<std::thread> threads;
    for (size_t i = 0; i < numThreads; i++) {
        threads.emplace_back([i, clauseSize, &r]() {
            for (size_t j = 0; j < 10; j++) {
                std::vector<int> lits(clauseSize, i);
                while (!r.insertClause(Clause{lits.data(), clauseSize, clauseSize}, i));
            }
        });
    }

    std::vector<int> out;
    while (out.size() < numThreads * 10 * clauseSize) {
        bool success = r.getClause(out);
        if (success) {
            log(V3_VERB, "Got %i lits\n", out.size());
        }
    }
    assert(!r.getClause(out));

    for (auto& thread : threads) thread.join();

    for (int lit : out) log(LOG_NO_PREFIX | V4_VVER, "%i ", lit);
    log(LOG_NO_PREFIX | V4_VVER, "\n");
}

void testUniformSizeClauseBuffer() {
    log(V2_INFO, "Testing uniform size clause ringbuffer ...\n");

    int clauseSize = 10;
    int numThreads = 100;
    UniformSizeClauseRingBuffer r(1000, clauseSize, numThreads);
    std::vector<std::thread> threads;
    for (size_t i = 0; i < numThreads; i++) {
        threads.emplace_back([i, clauseSize, &r]() {
            for (size_t j = 0; j < 10; j++) {
                std::vector<int> lits(clauseSize, i);
                while (!r.insertClause(Clause{lits.data(), clauseSize, /*lbd=*/(int)(j+1)}, i));
            }
        });
    }

    std::vector<int> out;
    while (out.size() < numThreads * 10 * (clauseSize+1)) {
        std::vector<int> buf;
        bool success = r.getClause(buf);
        if (success) {
            assert(buf.size() == clauseSize+1);
            out.insert(out.end(), buf.begin(), buf.end());
            log(V3_VERB, "Got %i lits\n", out.size());
        } else assert(buf.empty());
    }
    assert(!r.getClause(out));

    for (auto& thread : threads) thread.join();

    for (int lit : out) log(LOG_NO_PREFIX | V4_VVER, "%i ", lit);
    log(LOG_NO_PREFIX | V4_VVER, "\n");
}

int main() {
    Timer::init();
    Random::init(rand(), rand());
    Logger::init(0, V5_DEBG, false, false, false, nullptr);
    testMixedRingbuffer();
    testUnitBuffer();
    testUniformClauseBuffer();
    testUniformSizeClauseBuffer();
}
