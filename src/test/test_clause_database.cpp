
#include <iostream>
#include "util/assert.hpp"
#include <vector>
#include <string>
#include <thread>
#include <set>
#include <random>
#include <unistd.h>

#include "util/sys/process.hpp"
#include "util/sys/thread_pool.hpp"
#include "util/random.hpp"
#include "util/logger.hpp"
#include "util/sys/timer.hpp"
#include "app/sat/hordesat/sharing/lockfree_clause_database.hpp"
#include "app/sat/hordesat/sharing/adaptive_clause_database.hpp"
#include "app/sat/hordesat/sharing/buffer_merger.hpp"

bool insertUntilSuccess(AdaptiveClauseDatabase& cdb, size_t prodId, Clause& c) {
    auto result = AdaptiveClauseDatabase::AddClauseResult::TRY_LATER;
    while (result == AdaptiveClauseDatabase::AddClauseResult::TRY_LATER)
        result = cdb.addClause(prodId, c);
    return result == AdaptiveClauseDatabase::AddClauseResult::SUCCESS;
}

void testMerge() {
    LOG(V2_INFO, "Testing merge of clause buffers ...\n");

    int maxClauseSize = 3;
    int maxLbdPartitionedSize = 5;
    int numBuffers = 4;
    int numClausesPerBuffer = 100000;

    std::vector<std::vector<int>> buffers;
    std::vector<std::vector<int>> clauseLits;
    for (int i = 0; i < numBuffers; i++) {
        AdaptiveClauseDatabase cdb(maxClauseSize, maxLbdPartitionedSize, 1500, 1000, 1);
        for (int j = 0; j < numClausesPerBuffer; j++) {
            std::vector<int> lits;
            int clauseSize = 1 + (int)std::round(Random::rand() * (maxClauseSize-1));
            for (int l = 0; l < clauseSize; l++) {
                lits.push_back((Random::rand() < 0.5 ? -1 : 1) * (1 + Random::rand()*1000000));
            }
            clauseLits.push_back(std::move(lits));
            int glue = (int)std::round(2 + Random::rand()*(clauseSize-2));
            Clause c{clauseLits.back().data(), clauseSize, glue};
            //log(V5_DEBG, "CLS %s\n", c.toStr().c_str());
            assert(insertUntilSuccess(cdb, 0, c));
        }
        int numExported;
        auto buf = cdb.exportBuffer(100000, numExported);
        auto reader = cdb.getBufferReader(buf.data(), buf.size());
        Clause lastClause;
        BufferMerger::ClauseComparator compare;
        int clsIdx = 0;
        while (true) {
            auto cls = reader.getNextIncomingClause();
            if (cls.begin == nullptr) break;
            //log(V5_DEBG, "i=%i #%i %s\n", i, clsIdx, cls.toStr().c_str());
            if (lastClause.begin != nullptr) {
                assert(compare(lastClause, cls) || !compare(cls, lastClause) 
                    || LOG_RETURN_FALSE("%s > %s!\n", lastClause.toStr().c_str(), cls.toStr().c_str()));
            }
            lastClause = cls;
            clsIdx++;
        }
        buffers.push_back(std::move(buf));
    }

    AdaptiveClauseDatabase cdb(maxClauseSize, maxLbdPartitionedSize, 1500, 20, 1);
    LOG(V2_INFO, "Merging ...\n");
    auto merger = cdb.getBufferMerger();
    for (auto& buffer : buffers) merger.add(cdb.getBufferReader(buffer.data(), buffer.size()));
    std::vector<int> excess;
    auto merged = merger.merge(300000, &excess);
    LOG(V2_INFO, "Merged into buffer of size %ld, excess buffer has size %ld\n", merged.size(), excess.size());
}

void testUniform() {
    LOG(V2_INFO, "Testing lock-free clause database, uniform setting ...\n");

    int maxClauseSize = 30;
    int maxLbdPartitionedSize = 5;
    int baseBufferSize = 1000;
    int numProducers = 16;
    int numClauses = 1000;

    AdaptiveClauseDatabase cdb(maxClauseSize, maxLbdPartitionedSize, baseBufferSize, 20, numProducers);

    // Create stream of clauses into database
    std::vector<std::thread> threads(numProducers);
    for (size_t i = 0; i < threads.size(); i++) {
        threads[i] = std::thread([i, &cdb, maxLbdPartitionedSize, maxClauseSize]() {
            BucketLabel b;
            for (size_t j = 0; j < 2*i; j++) b.next(maxLbdPartitionedSize);
            if (b.size <= maxLbdPartitionedSize) {
                std::vector<int> lits;
                for (size_t j = 0; j < b.size; j++) lits.push_back(100*b.size+b.lbd);
                Clause c{lits.data(), b.size, b.lbd};
                assert(insertUntilSuccess(cdb, i, c));
            } else {
                for (int lbd = 1; lbd <= 3; lbd++) {
                    for (int crep = 0; crep < 1; crep++) {
                        std::vector<int> lits;
                        for (size_t j = 0; j < b.size; j++) lits.push_back(100*b.size+lbd);
                        Clause c{lits.data(), b.size, lbd};
                        assert(insertUntilSuccess(cdb, i, c) == (b.size <= maxClauseSize) 
                            || LOG_RETURN_FALSE("Failed to add cls of size %i, lbd %i\n", b.size, lbd));
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

    for (int lit : out) LOG_OMIT_PREFIX(V4_VVER, "%i ", lit);
    LOG_OMIT_PREFIX(V4_VVER, "\n");

    auto reader = cdb.getBufferReader(out.data(), out.size());
    Clause c = reader.getNextIncomingClause();
    while (c.begin != nullptr) {
        LOG_OMIT_PREFIX(V4_VVER, "lbd=%i ", c.lbd);
        for (size_t i = 0; i < c.size; i++) LOG_OMIT_PREFIX(V4_VVER, "%i ", c.begin[i]);
        LOG_OMIT_PREFIX(V4_VVER, "0\n");
        c = reader.getNextIncomingClause();
    }
    LOG_OMIT_PREFIX(V4_VVER, "\n");

    auto merger = cdb.getBufferMerger();
    for (int n = 0; n < 10; n++) merger.add(cdb.getBufferReader(out.data(), out.size()));
    auto merged = merger.merge(10000);

    for (int lit : merged) LOG_OMIT_PREFIX(V4_VVER, "%i ", lit);
    LOG_OMIT_PREFIX(V4_VVER, "\n");

    reader = cdb.getBufferReader(merged.data(), merged.size());
    c = reader.getNextIncomingClause();
    while (c.begin != nullptr) {
        LOG_OMIT_PREFIX(V4_VVER, "lbd=%i ", c.lbd);
        for (size_t i = 0; i < c.size; i++) LOG_OMIT_PREFIX(V4_VVER, "%i ", c.begin[i]);
        LOG_OMIT_PREFIX(V4_VVER, "0\n");
        c = reader.getNextIncomingClause();
    }

    assert(merged == out);

    LOG(V2_INFO, "Done.\n");
}

void testRandomClauses() {
    LOG(V2_INFO, "Testing lock-free clause database ...\n");

    int maxClauseSize = 30;
    int maxLbdPartitionedSize = 5;
    int baseBufferSize = 1500;
    int numProducers = 4;
    int numClauses = 10000;

    LOG(V2_INFO, "Generating %i clauses ...\n", numClauses);

    // Generate clauses
    std::vector<std::vector<int>> inputClauses;
    ExactSortedClauseFilter filter;
    int numDistinct = 0;
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
        std::sort(cls.begin()+1, cls.end());
        assert(cls.size() == 1+len);
        Clause c((int*)malloc(sizeof(int)*(cls.size()-1)), cls.size()-1, lbd);
        memcpy(c.begin, cls.data()+1, sizeof(int)*(cls.size()-1));
        if (filter.registerClause(c)) numDistinct++;
        inputClauses.push_back(std::move(cls));
    }

    LOG(V2_INFO, "Generated %i distinct clauses.\n", numDistinct);

    LOG(V2_INFO, "Setting up clause database ...\n");

    AdaptiveClauseDatabase cdb(maxClauseSize, maxLbdPartitionedSize, baseBufferSize, 20, numProducers);
    std::vector<std::vector<int>> buffers;

    LOG(V2_INFO, "Performing multi-threaded addition of clauses ...\n");

    for (size_t rep = 0; rep < 3; rep++) {

        // Create stream of clauses into database
        std::vector<std::thread> threads(numProducers);
        std::atomic_int numAdded = 0;
        std::atomic_int numAddedLiterals = 0;
        std::atomic_int numRejected = 0;
        for (size_t i = 0; i < threads.size(); i++) {
            threads[i] = std::thread([i, &inputClauses, &cdb, &numAdded, &numRejected]() {
                auto rng = std::mt19937(i);
                auto dist = std::uniform_real_distribution<float>(0, 1);
                for (int n = 0; n < 10000; n++) {
                    auto& clause = inputClauses[(int) (dist(rng) * inputClauses.size())];
                    Clause c{clause.data()+1, (int)clause.size()-1, clause[0]};
                    //log(V2_INFO, "add cls %s\n", c.toStr().c_str());
                    auto result = cdb.addClause(i, c);
                    (result == AdaptiveClauseDatabase::SUCCESS ? numAdded : numRejected)++;
                }
            });
        }

        // Wait for streams to finish
        for (auto& thread : threads) thread.join();

        // Export buffer from database
        int numExported = 0;
        auto out = cdb.exportBuffer(1000000, numExported);
        LOG(V2_INFO, " - %i/%i added, %i exported into buffer\n", (int)numAdded, (int)numRejected, numExported);

        //for (int lit : out) LOG_OMIT_PREFIX(V4_VVER, "%i ", lit);
        //log(LOG_NO_PREFIX | V4_VVER, "\n");
        buffers.push_back(std::move(out));
    }

    auto merger = cdb.getBufferMerger();
    for (auto& out : buffers) merger.add(cdb.getBufferReader(out.data(), out.size()));

    LOG(V2_INFO, "Merging buffers ...\n");
    auto merged = merger.merge(1000000);
    LOG(V2_INFO, "Merged buffers into buffer of size %i\n", merged.size());
    
    //for (int lit : merged) LOG_OMIT_PREFIX(V4_VVER, "%i ", lit);
    //log(LOG_NO_PREFIX | V4_VVER, "\n");

    auto reader = cdb.getBufferReader(merged.data(), merged.size());
    Clause c = reader.getNextIncomingClause();
    int readClauses = 0;
    while (c.begin != nullptr) {
        readClauses++;
        //log(LOG_NO_PREFIX | V4_VVER, "lbd=%i ", c.lbd);
        //for (size_t i = 0; i < c.size; i++) LOG_OMIT_PREFIX(V4_VVER, "%i ", c.begin[i]);
        //log(LOG_NO_PREFIX | V4_VVER, "0\n");
        c = reader.getNextIncomingClause();
    }

    LOG(V3_VERB, "Read %i clauses from merged buffer\n", readClauses);
}

Clause produceClause(std::function<float()> normalRng, float meanLength) {
    meanLength = std::max(0.0f, meanLength);
    Clause c;
    c.size = 1 + std::max(0, (int)std::round(2 * (meanLength-0.5) * normalRng()));
    c.size = std::min(c.size, (int)(2*meanLength));
    assert(c.size > 0);
    
    //c.size = 1;
    c.begin = (int*)malloc(c.size*sizeof(int));
    for (size_t j = 0; j < c.size; j++) {
        c.begin[j] = (int)(Random::rand()*20000+1);
        if (Random::rand() < 0.5) c.begin[j] = -c.begin[j];
    }
    c.lbd = 2+(int)(Random::rand()*(c.size-1));
    return c;
}

void testConcurrentClauseAddition() {
    LOG(V2_INFO, "Testing lock-free clause database ...\n");

    int maxClauseSize = 30;
    int maxLbdPartitionedSize = 5;
    int baseBufferSize = 1500;
    int numProducers = 4;
    int numClausesPerThread = 100000;

    AdaptiveClauseDatabase cdb(maxClauseSize, maxLbdPartitionedSize, baseBufferSize, 20, numProducers);

    std::atomic_int sumOfProducedLengths = 0;
    std::atomic_int numFinished = 0;

    std::vector<std::thread> threads(numProducers);
    for (size_t i = 0; i < numProducers; i++) {
        threads[i] = std::thread([i, &numClausesPerThread, &cdb, &sumOfProducedLengths, &numFinished, maxLbdPartitionedSize]() {
            std::default_random_engine generator(i);
            std::normal_distribution<float> distribution(0.5, 0.5);
            auto rng = [&]() {return distribution(generator);};

            std::vector<Clause> deferredClauses;

            int produced = 0;
            int initiallyPassed = 0;
            int passed = 0;
            int dropped = 0;
            int deferred = 0;
            while (produced < numClausesPerThread || !deferredClauses.empty()) {
                
                Clause c;
                if (produced < numClausesPerThread) {
                    float doneRatio = (float)produced / numClausesPerThread;
                    float meanLength = 1;
                    if (doneRatio <= 0.5) meanLength += doneRatio * 15;
                    else meanLength += (1-doneRatio) * 15;
                    
                    c = produceClause(rng, meanLength);
                } else {
                    c = deferredClauses.back();
                    deferredClauses.pop_back();
                }

                auto result = cdb.addClause(i, c);

                if (result == AdaptiveClauseDatabase::TRY_LATER) {
                    deferredClauses.push_back(c);
                    deferred++;
                } else {
                    free(c.begin);
                    (result == AdaptiveClauseDatabase::SUCCESS ? passed : dropped)++;
                }

                produced++;
                sumOfProducedLengths += c.size + (c.size > maxLbdPartitionedSize ? 1 : 0);
                if (produced == numClausesPerThread) {
                    initiallyPassed = passed;
                }
                usleep(1000);
            }

            LOG(V2_INFO, "Thread %i : %i passed, %i dropped, %i deferred (%.4f initially, %.4f eventually passed)\n", 
                i, passed, dropped, deferred, ((float)initiallyPassed)/produced, ((float)passed)/produced);
            numFinished++;
        });
    }

    const int sizePerExport = 10*baseBufferSize;

    int totalExported = 0;
    int totalBufsize = 0;
    std::vector<std::vector<int>> buffers;
    bool continueExporting = true;
    while (true) {
        usleep(1000 * 1000); // 1s
        //usleep(1000);

        if (numFinished == numProducers) continueExporting = false;
        
        cdb.printChunks(/*nextExportSize=*/sizePerExport);

        int numExported = 0;
        buffers.push_back(cdb.exportBuffer(sizePerExport, numExported));
        if (numExported == 0 && !continueExporting) break;
        totalBufsize += buffers.back().size();
        totalExported += numExported;
        float meanLength = ((float)buffers.back().size()/numExported);

        LOG(V2_INFO, "Exported %i clauses, bufsize %i (mean length %.3f)\n", numExported, buffers.back().size(), meanLength);
    }

    float producedMeanLength = ((int)sumOfProducedLengths / (float)(numProducers*numClausesPerThread));
    float exportedMeanLength = totalBufsize / (float)totalExported;
    LOG(V2_INFO, "Exported %i clauses in total (%.3f dropped or left back)\n", totalExported, 1 - (float)totalExported / (numProducers*numClausesPerThread));
    LOG(V2_INFO, "Produced mean length: %.3f ; exported mean length: %.3f\n", producedMeanLength, exportedMeanLength);
 
    for (auto& thread : threads) thread.join();
}

int main() {
    Timer::init();
    Random::init(rand(), rand());
    Logger::init(0, V5_DEBG, false, false, false, nullptr);
    Process::init(0);
    ProcessWideThreadPool::init(1);

    testMerge();
    testUniform();
    testRandomClauses();
    //testConcurrentClauseAddition();
}
