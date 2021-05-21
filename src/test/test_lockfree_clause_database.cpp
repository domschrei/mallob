
#include <iostream>
#include <assert.h>
#include <vector>
#include <string>
#include <thread>
#include <set>

#include "util/random.hpp"
#include "util/logger.hpp"
#include "util/sys/timer.hpp"
#include "app/sat/hordesat/sharing/lockfree_clause_database.hpp"

void testUniform() {
    log(V2_INFO, "Testing lock-free clause database, uniform setting ...\n");

    int maxClauseSize = 30;
    int maxLbdPartitionedSize = 5;
    int baseBufferSize = 1000;
    int numProducers = 16;
    int numClauses = 1000;


    LockfreeClauseDatabase cdb(maxClauseSize, maxLbdPartitionedSize, baseBufferSize, numProducers);

    // Create stream of clauses into database
    std::vector<std::thread> threads(numProducers);
    for (size_t i = 0; i < threads.size(); i++) {
        threads[i] = std::thread([i, &cdb, maxLbdPartitionedSize, maxClauseSize]() {
            LockfreeClauseDatabase::BucketLabel b;
            for (size_t j = 0; j < 2*i; j++) b.next(maxLbdPartitionedSize);
            if (b.size <= maxLbdPartitionedSize) {
                std::vector<int> lits;
                for (size_t j = 0; j < b.size; j++) lits.push_back(100*b.size+b.lbd);
                bool success = cdb.addClause(i, Clause{lits.data(), b.size, b.lbd});
                assert(success);
            } else {
                for (int lbd = 1; lbd <= 3; lbd++) {
                    for (int crep = 0; crep < 1; crep++) {
                        std::vector<int> lits;
                        for (size_t j = 0; j < b.size; j++) lits.push_back(100*b.size+lbd);
                        bool success = cdb.addClause(i, Clause{lits.data(), b.size, lbd});
                        assert(success == (b.size <= maxClauseSize) || log_return_false("Failed to add cls of size %i, lbd %i\n", b.size, lbd));
                    }
                }
            }
        });
    }

    /*
    int totalSize = 0;
    LockfreeClauseDatabase::BucketLabel b;
    for (size_t i = 0; i < numProducers; i++) {
        totalSize += b.size;
        b.next(maxLbdPartitionedSize);
    }
    */

    // Wait for streams to finish
    for (auto& thread : threads) thread.join();


    // Export buffer from database

    int numExported = 0;
    auto out = cdb.exportBuffer(1000000, numExported);
    //assert(numExported == numProducers);

    for (int lit : out) log(LOG_NO_PREFIX | V4_VVER, "%i ", lit);
    log(LOG_NO_PREFIX | V4_VVER, "\n");

    auto reader = cdb.getBufferReader(out.data(), out.size());
    Clause c = reader.getNextIncomingClause();
    while (c.begin != nullptr) {
        log(LOG_NO_PREFIX | V4_VVER, "lbd=%i ", c.lbd);
        for (size_t i = 0; i < c.size; i++) log(LOG_NO_PREFIX | V4_VVER, "%i ", c.begin[i]);
        log(LOG_NO_PREFIX | V4_VVER, "0\n");
        c = reader.getNextIncomingClause();
    }
    log(LOG_NO_PREFIX | V4_VVER, "\n");


    auto merger = cdb.getBufferMerger();
    for (int n = 0; n < 10; n++) merger.add(cdb.getBufferReader(out.data(), out.size()));
    auto merged = merger.merge(10000);

    for (int lit : merged) log(LOG_NO_PREFIX | V4_VVER, "%i ", lit);
    log(LOG_NO_PREFIX | V4_VVER, "\n");

    reader = cdb.getBufferReader(merged.data(), merged.size());
    c = reader.getNextIncomingClause();
    while (c.begin != nullptr) {
        log(LOG_NO_PREFIX | V4_VVER, "lbd=%i ", c.lbd);
        for (size_t i = 0; i < c.size; i++) log(LOG_NO_PREFIX | V4_VVER, "%i ", c.begin[i]);
        log(LOG_NO_PREFIX | V4_VVER, "0\n");
        c = reader.getNextIncomingClause();
    }

    assert(merged == out);
}

void testRandomClauses() {
    log(V2_INFO, "Testing lock-free clause database ...\n");

    int maxClauseSize = 30;
    int maxLbdPartitionedSize = 5;
    int baseBufferSize = 10000;
    int numProducers = 100;
    int numClauses = 100;

    // Generate clauses
    std::vector<std::vector<int>> inputClauses;
    for (int i = 0; i < numClauses; i++) {
        int len = (int) (maxClauseSize * Random::rand()) + 1;
        int lbd = std::min(len, (int) ((len-1) * Random::rand()) + 2);
        assert(lbd >= 1 && lbd <= len);
        std::vector<int> cls(1, lbd);
        for (int x = 0; x < len; x++) {
            int lit = (int) (10000 * Random::rand()) + 1;
            if (Random::rand() < 0.5) lit *= -1;
            assert(lit != 0);
            cls.push_back(lit);
        }
        assert(cls.size() == 1+len);
        inputClauses.push_back(std::move(cls));
    }


    LockfreeClauseDatabase cdb(maxClauseSize, maxLbdPartitionedSize, baseBufferSize, numProducers);


    std::vector<std::vector<int>> buffers;

    for (size_t rep = 0; rep < 1; rep++) {

        // Create stream of clauses into database
        std::vector<std::thread> threads(numProducers);
        for (size_t i = 0; i < threads.size(); i++) {
            threads[i] = std::thread([i, &inputClauses, &cdb]() {
                for (int n = 0; n < 10; n++) {
                    auto& clause = inputClauses[(int) (Random::rand() * inputClauses.size())];
                    bool success = cdb.addClause(i, Clause{clause.data()+1, (int)clause.size()-1, clause[0]});
                }
            });
        }

        // Wait for streams to finish
        for (auto& thread : threads) thread.join();

        // Export buffer from database
        int numExported = 0;
        auto out = cdb.exportBuffer(1000000, numExported);
        //for (int lit : out) log(LOG_NO_PREFIX | V4_VVER, "%i ", lit);
        //log(LOG_NO_PREFIX | V4_VVER, "\n");
        buffers.push_back(std::move(out));
    }


    auto merger = cdb.getBufferMerger();
    for (auto& out : buffers) merger.add(cdb.getBufferReader(out.data(), out.size()));
    auto merged = merger.merge(10000);

    for (int lit : merged) log(LOG_NO_PREFIX | V4_VVER, "%i ", lit);
    log(LOG_NO_PREFIX | V4_VVER, "\n");

    auto reader = cdb.getBufferReader(merged.data(), merged.size());
    Clause c = reader.getNextIncomingClause();
    int readClauses = 0;
    while (c.begin != nullptr) {
        readClauses++;
        log(LOG_NO_PREFIX | V4_VVER, "lbd=%i ", c.lbd);
        for (size_t i = 0; i < c.size; i++) log(LOG_NO_PREFIX | V4_VVER, "%i ", c.begin[i]);
        log(LOG_NO_PREFIX | V4_VVER, "0\n");
        c = reader.getNextIncomingClause();
    }

    log(V3_VERB, "%i clauses\n", readClauses);

    //assert(merged == out);
}

int main() {
    Timer::init();
    Random::init(rand(), rand());
    Logger::init(0, V5_DEBG, false, false, false, nullptr);
    testRandomClauses();
}
