
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
#include "app/sat/sharing/buffer/adaptive_clause_database.hpp"
#include "app/sat/sharing/buffer/buffer_merger.hpp"
#include "app/sat/sharing/buffer/buffer_reducer.hpp"
#include "util/sys/terminator.hpp"

bool insertUntilSuccess(AdaptiveClauseDatabase& cdb, size_t prodId, Clause& c) {
    return cdb.addClause(c);
}

void testSumBucketLabel() {
    LOG(V2_INFO, "Testing sum-mode bucket label ...\n");
    BucketLabel l(BucketLabel::MINIMIZE_SUM_OF_SIZE_AND_LBD, /*maxLbdPartitionedSize=*/5);
    int maxClauseSize = 20;
    while (l.size+l.lbd <= maxClauseSize+2) {
        LOG(V2_INFO, "size=%i lbd=%i\n", l.size, l.lbd);
        l.next();
    }
}

void testMinimal() {
    LOG(V2_INFO, "Minimal test ...\n");

    AdaptiveClauseDatabase::Setup setup;
    setup.maxClauseLength = 10;
    setup.maxLbdPartitionedSize = 5;
    setup.numLiterals = 1500;
    setup.slotsForSumOfLengthAndLbd = true;

    AdaptiveClauseDatabase cdb(setup);
    bool success = true;
    int numAttempted = 0;
    cdb.checkTotalLiterals();

    while (success && numAttempted < 10000) {
        std::vector<int> lits = {100+numAttempted, 200+numAttempted, 300+numAttempted, 400+numAttempted};
        LOG(V2_INFO, "%i\n", numAttempted);
        Clause c{lits.data(), lits.size(), 2};
        success = cdb.addClause(c);
        numAttempted++;
        cdb.checkTotalLiterals();
    }
    assert(numAttempted > 1);
    assert(numAttempted < 10000);

    int numExported;
    auto buf = cdb.exportBuffer(100000, numExported);

    std::string out = "";
    for (int lit : buf) out += std::to_string(lit) + " ";
    LOG(V2_INFO, "BUF: %s\n", out.c_str());

    cdb.checkTotalLiterals();
}

void testMerge() {
    LOG(V2_INFO, "Testing merge of clause buffers ...\n");

    AdaptiveClauseDatabase::Setup setup;
    setup.maxClauseLength = 3;
    setup.maxLbdPartitionedSize = 5;
    setup.numLiterals = 1'500'000;
    setup.slotsForSumOfLengthAndLbd = true;
    int numBuffers = 4;
    int numClausesPerBuffer = 100000;

    std::vector<std::vector<int>> buffers;
    std::vector<std::vector<int>> clauseLits;
    for (int i = 0; i < numBuffers; i++) {

        AdaptiveClauseDatabase cdb(setup);
        for (int j = 0; j < numClausesPerBuffer; j++) {
            std::vector<int> lits;
            int clauseSize = 1 + (int)std::round(Random::rand() * (setup.maxClauseLength-1));
            for (int l = 0; l < clauseSize; l++) {
                lits.push_back((Random::rand() < 0.5 ? -1 : 1) * (1 + Random::rand()*1000000));
            }
            clauseLits.push_back(std::move(lits));
            int glue = (int)std::round(2 + Random::rand()*(clauseSize-2));
            glue = std::min(glue, clauseSize);
            Clause c{clauseLits.back().data(), clauseSize, glue};
            //log(V5_DEBG, "CLS #%i %s\n", j, c.toStr().c_str());
            insertUntilSuccess(cdb, 0, c);
        }
        
        int numExported;
        auto buf = cdb.exportBuffer(100000, numExported);
        cdb.checkTotalLiterals();

        auto reader = cdb.getBufferReader(buf.data(), buf.size());
        Clause lastClause;
        ClauseComparator compare(setup.slotsForSumOfLengthAndLbd ?
            (AbstractClauseThreewayComparator*) new LengthLbdSumClauseThreewayComparator(setup.maxClauseLength+2) :
            (AbstractClauseThreewayComparator*) new LexicographicClauseThreewayComparator()
        );;
        int clsIdx = 0;
        while (true) {
            auto cls = reader.getNextIncomingClause();
            if (cls.begin == nullptr) break;
            //log(V5_DEBG, "i=%i #%i %s\n", i, clsIdx, cls.toStr().c_str());
            if (lastClause.begin != nullptr) {
                assert(compare(lastClause, cls) || !compare(cls, lastClause) 
                    || log_return_false("%s > %s!\n", lastClause.toStr().c_str(), cls.toStr().c_str()));
            }
            lastClause = cls;
            clsIdx++;
        }
        buffers.push_back(std::move(buf));
        LOG(V2_INFO, "Buffer %i : exported %i clauses\n", i, clsIdx);
    }

    setup.numLiterals = 30'000;
    AdaptiveClauseDatabase cdb(setup);
    LOG(V2_INFO, "Merging ...\n");
    auto merger = cdb.getBufferMerger(300000);
    for (auto& buffer : buffers) merger.add(cdb.getBufferReader(buffer.data(), buffer.size()));
    std::vector<int> excess;
    auto merged = merger.merge(&excess);
    LOG(V2_INFO, "Merged into buffer of size %ld, excess buffer has size %ld\n", merged.size(), excess.size());

    auto mergedReader = cdb.getBufferReader(merged.data(), merged.size());
    std::unordered_map<std::pair<int, int>, int, IntPairHasher> lengthLbdToNumOccs;
    while (true) {
        auto& c = mergedReader.getNextIncomingClause();
        if (c.begin == nullptr) break;
        lengthLbdToNumOccs[std::pair<int, int>(c.size, c.lbd)]++;
    }
    for (auto& [pair, num] : lengthLbdToNumOccs) {
        auto [len, lbd] = pair;
        LOG(V2_INFO, "#clauses (%i,%i) : %i\n", len, lbd, num);
    }

    /*
    {
        std::string out = "";
        for (int lit : merged) out += std::to_string(lit) + " ";
        LOG(V2_INFO, "MERGED: %s\n", out.c_str());
    }
    {
        std::string out = "";
        for (int lit : excess) out += std::to_string(lit) + " ";
        LOG(V2_INFO, "EXCESS: %s\n", out.c_str());
    }*/
}

void testReduce() {

    LOG(V2_INFO, "Test in-place buffer reduction ...\n");

    AdaptiveClauseDatabase::Setup setup;
    setup.maxClauseLength = 10;
    setup.maxLbdPartitionedSize = 5;
    setup.numLiterals = 1500;
    setup.slotsForSumOfLengthAndLbd = true;

    AdaptiveClauseDatabase cdb(setup);
    bool success = true;
    
    const int nbMaxClausesPerSlot = 10;
    int nbClausesThisSlot = 0;
    BufferIterator it(setup.maxClauseLength, setup.slotsForSumOfLengthAndLbd);

    while (success) {
        std::vector<int> lits(it.clauseLength);
        for (size_t i = 0; i < lits.size(); i++) {
            lits[i] = 100*nbClausesThisSlot + i + 1;
        }
        Clause c{lits.data(), lits.size(), it.lbd};
        success = cdb.addClause(c);
        nbClausesThisSlot++;
        if (nbClausesThisSlot == nbMaxClausesPerSlot) {
            it.nextLengthLbdGroup();
            nbClausesThisSlot = 0;
        }
    }

    cdb.checkTotalLiterals();
    int numExported;
    auto buf = cdb.exportBuffer(100000, numExported);
    cdb.checkTotalLiterals();

    {
        auto copiedBuf = buf;
        BufferReducer reducer(copiedBuf.data(), copiedBuf.size(), setup.maxClauseLength, setup.slotsForSumOfLengthAndLbd);
        int newSize = reducer.reduce([&](const Mallob::Clause& c) {
            return true;
        });
        copiedBuf.resize(newSize);
        assert(copiedBuf == buf);

        BufferReader reader(copiedBuf.data(), copiedBuf.size(), setup.maxClauseLength, setup.slotsForSumOfLengthAndLbd);
        int nbClauses = 0;
        while (reader.getNextIncomingClause().begin != nullptr) nbClauses++;
        assert(nbClauses == numExported);
    }
    {
        auto copiedBuf = buf;
        BufferReducer reducer(copiedBuf.data(), copiedBuf.size(), setup.maxClauseLength, setup.slotsForSumOfLengthAndLbd);
        int newSize = reducer.reduce([&](const Mallob::Clause& c) {
            return false;
        });
        copiedBuf.resize(newSize);

        BufferReader reader(copiedBuf.data(), copiedBuf.size(), setup.maxClauseLength, setup.slotsForSumOfLengthAndLbd);
        int nbClauses = 0;
        while (reader.getNextIncomingClause().begin != nullptr) nbClauses++;
        assert(nbClauses == 0);
    }
    {
        auto copiedBuf = buf;
        BufferReducer reducer(copiedBuf.data(), copiedBuf.size(), setup.maxClauseLength, setup.slotsForSumOfLengthAndLbd);
        auto reductor = [&](const Mallob::Clause& c) {
            return c.size == 4 && c.lbd == 3 && c.begin[0] == 1 && c.begin[1] == 2 && c.begin[2] == 3 && c.begin[3] == 4;
        };
        int newSize = reducer.reduce(reductor);
        copiedBuf.resize(newSize);

        BufferReader reader(copiedBuf.data(), copiedBuf.size(), setup.maxClauseLength, setup.slotsForSumOfLengthAndLbd);
        int nbClauses = 0;
        while (reader.getNextIncomingClause().begin != nullptr) {
            nbClauses++;
            assert(reductor(*reader.getCurrentClausePointer()));
        }
        assert(nbClauses == 1);
    }
    {
        auto copiedBuf = buf;
        BufferReducer reducer(copiedBuf.data(), copiedBuf.size(), setup.maxClauseLength, setup.slotsForSumOfLengthAndLbd);
        auto reductor = [&](const Mallob::Clause& c) {
            return c.size >= 4;
        };
        int newSize = reducer.reduce(reductor);
        copiedBuf.resize(newSize);

        BufferReader reader(copiedBuf.data(), copiedBuf.size(), setup.maxClauseLength, setup.slotsForSumOfLengthAndLbd);
        int nbClauses = 0;
        while (reader.getNextIncomingClause().begin != nullptr) {
            nbClauses++;
            assert(reductor(*reader.getCurrentClausePointer()));
        }
        assert(nbClauses == numExported - 40);
    }

    //std::string out = "";
    //for (int lit : buf) out += std::to_string(lit) + " ";
    //LOG(V2_INFO, "BUF: %s\n", out.c_str());
}

int main() {
    Timer::init();
    Random::init(rand(), rand());
    Logger::init(0, V5_DEBG, false, false, false, nullptr);
    Process::init(0);
    ProcessWideThreadPool::init(1);

    testSumBucketLabel();
    testMinimal();
    testMerge();
    testReduce();
}


#ifdef MALLOB_COMMENTED_OUT
void testUniform() {
    LOG(V2_INFO, "Testing lock-free clause database, uniform setting ...\n");

    AdaptiveClauseDatabase::Setup setup;
    setup.maxClauseLength = 30;
    setup.maxLbdPartitionedSize = 5;
    setup.numProducers = 16;
    setup.numLiterals = 200'000;
    setup.slotsForSumOfLengthAndLbd = false;
    int numClauses = 1000;

    AdaptiveClauseDatabase cdb(setup);

    // Create stream of clauses into database
    std::vector<std::thread> threads(setup.numProducers);
    for (size_t i = 0; i < threads.size(); i++) {
        threads[i] = std::thread([i, &cdb, setup]() {
            BufferIterator it(setup.maxClauseLength, setup.slotsForSumOfLengthAndLbd);
            for (size_t j = 0; j < i; j++) it.nextLengthLbdGroup();
            std::vector<int> lits;
            for (size_t j = 0; j < it.clauseLength; j++) lits.push_back(100*it.clauseLength+it.lbd);
            Clause c{lits.data(), it.clauseLength, it.lbd};
            assert(insertUntilSuccess(cdb, i, c));
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

    auto merger = cdb.getBufferMerger(10000);
    for (int n = 0; n < 10; n++) merger.add(cdb.getBufferReader(out.data(), out.size()));
    auto merged = merger.merge();

    for (int lit : merged) LOG_OMIT_PREFIX(V4_VVER, "%i ", lit);
    LOG_OMIT_PREFIX(V4_VVER, "\n");

    auto reader2 = cdb.getBufferReader(merged.data(), merged.size());
    c = reader2.getNextIncomingClause();
    while (c.begin != nullptr) {
        LOG_OMIT_PREFIX(V4_VVER, "lbd=%i ", c.lbd);
        for (size_t i = 0; i < c.size; i++) LOG_OMIT_PREFIX(V4_VVER, "%i ", c.begin[i]);
        LOG_OMIT_PREFIX(V4_VVER, "0\n");
        c = reader2.getNextIncomingClause();
    }

    assert(merged == out);

    LOG(V2_INFO, "Done.\n");
}

void testRandomClauses() {
    LOG(V2_INFO, "Testing random clauses ...\n");

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
        if (len > 1) lbd = std::max(2, lbd);
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

    AdaptiveClauseDatabase::Setup setup;
    setup.maxClauseLength = maxClauseSize;
    setup.maxLbdPartitionedSize = maxLbdPartitionedSize;
    setup.numLiterals = 20*baseBufferSize;
    setup.numProducers = numProducers;
    setup.slotsForSumOfLengthAndLbd = true;
    AdaptiveClauseDatabase cdb(setup);
    std::vector<std::vector<int>> buffers;

    LOG(V2_INFO, "Performing multi-threaded addition of clauses ...\n");

    for (size_t rep = 0; rep < 3; rep++) {

        // Create stream of clauses into database
        std::vector<std::thread> threads(numProducers);
        std::atomic_int numAdded = 0;
        std::atomic_int numRejected = 0;
        std::atomic_int numDuplicates = 0;
        for (size_t i = 0; i < threads.size(); i++) {
            threads[i] = std::thread([i, rep, &inputClauses, &cdb, &numAdded, &numDuplicates, &numRejected]() {
                auto rng = std::mt19937(1000*rep + i);
                auto dist = std::uniform_real_distribution<float>(0, 1);
                for (int n = 0; n < 10000; n++) {
                    auto& clause = inputClauses[(int) (dist(rng) * inputClauses.size())];
                    Clause c{clause.data()+1, (int)clause.size()-1, clause[0]};
                    //log(V2_INFO, "add cls %s\n", c.toStr().c_str());
                    assert(c.lbd <= c.size);
                    auto result = cdb.addClause(i, c);
                    if (result == SUCCESS) numAdded++;
                    if (result == DUPLICATE) numDuplicates++;
                    if (result == NO_BUDGET) numRejected++;
                }
            });
        }

        // Wait for streams to finish
        for (auto& thread : threads) thread.join();
        LOG(V2_INFO, " - %i/%i literals in CDB\n", cdb.getCurrentlyUsedLiterals(), setup.numLiterals);

        // Export buffer from database
        int numExported = 0;
        auto out = cdb.exportBuffer(1000000, numExported);
        LOG(V2_INFO, " - %i/%i/%i added/rejected/duplicate, %i exported into buffer of size %i\n", 
            (int)numAdded, (int)numRejected, (int)numDuplicates, numExported, out.size());
        LOG(V2_INFO, " - %i/%i literals remaining in CDB\n", cdb.getCurrentlyUsedLiterals(), setup.numLiterals);

        //for (int lit : out) LOG_OMIT_PREFIX(V4_VVER, "%i ", lit);
        //log(LOG_NO_PREFIX | V4_VVER, "\n");
        buffers.push_back(std::move(out));
    }

    auto merger = cdb.getBufferMerger(1000000);
    for (auto& out : buffers) merger.add(cdb.getBufferReader(out.data(), out.size()));

    LOG(V2_INFO, "Merging buffers ...\n");
    auto merged = merger.merge();
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
    c.lbd = std::min(c.lbd, c.size);
    return c;
}

void testTreeMapVsHashMap() {

    std::default_random_engine generator(1);
    std::normal_distribution<float> distribution(0.5, 0.5);
    auto rng = [&]() {return distribution(generator);};

    std::vector<Clause> clauses;
    for (size_t i = 0; i < 10000; i++) {
        auto c = produceClause(rng, 5);
        while (c.size < 3) c = produceClause(rng, 5);
        clauses.push_back(c);
    }

    std::map<ProducedLargeClause, int> treeMap;
    tsl::robin_map<ProducedLargeClause, int, ProducedClauseHasher<ProducedLargeClause>, 
        ProducedClauseEqualsIgnoringLBD<ProducedLargeClause>> hashMap;
    float time;

    time = Timer::elapsedSeconds();
    for (auto& c : clauses) {
        treeMap.insert({ProducedLargeClause(c), 0});
    }
    LOG(V2_INFO, "10000 clauses in tree map: %.5fs\n", Timer::elapsedSeconds()-time);

    time = Timer::elapsedSeconds();
    for (auto& c : clauses) {
        hashMap.insert({ProducedLargeClause(c), 0});
    }
    LOG(V2_INFO, "10000 clauses in hash map: %.5fs\n", Timer::elapsedSeconds()-time);
}

void testConcurrentClauseAddition() {
    LOG(V2_INFO, "Testing concurrent clause addition ...\n");

    int maxClauseSize = 30;
    int maxLbdPartitionedSize = 5;
    int baseBufferSize = 1500;
    int numProducers = 4;
    int numClausesPerThread = 100000;

    AdaptiveClauseDatabase::Setup setup;
    setup.maxClauseLength = maxClauseSize;
    setup.maxLbdPartitionedSize = maxLbdPartitionedSize;
    setup.numLiterals = 20*baseBufferSize;
    setup.numProducers = numProducers;
    setup.slotsForSumOfLengthAndLbd = true;
    AdaptiveClauseDatabase cdb(setup);

    std::atomic_int sumOfProducedLengths = 0;
    std::atomic_int numFinished = 0;

    std::vector<std::thread> threads(numProducers);
    for (size_t i = 0; i < numProducers; i++) {
        threads[i] = std::thread([i, &numClausesPerThread, &cdb, &sumOfProducedLengths, &numFinished, maxLbdPartitionedSize]() {
            std::default_random_engine generator(i);
            std::normal_distribution<float> distribution(0.5, 0.5);
            auto rng = [&]() {return distribution(generator);};

            int produced = 0;
            int initiallyPassed = 0;
            int passed = 0;
            int dropped = 0;
            int deferred = 0;
            while (produced < numClausesPerThread) {
                
                if (Terminator::isTerminating()) break;

                Clause c;
                float doneRatio = (float)produced / numClausesPerThread;
                float meanLength = 1;
                if (doneRatio <= 0.5) meanLength += doneRatio * 15;
                else meanLength += (1-doneRatio) * 15;
                
                c = produceClause(rng, meanLength);

                bool success = cdb.addClause(i, c) == SUCCESS;
                free(c.begin);
                (success ? passed : dropped)++;

                produced++;
                sumOfProducedLengths += c.size + (c.size > maxLbdPartitionedSize ? 1 : 0);
                if (produced == numClausesPerThread) {
                    initiallyPassed = passed;
                }
                
                usleep(1000);
            }

            LOG(V2_INFO, "Thread %i : %i passed, %i dropped (%.4f passed)\n", 
                i, passed, dropped, ((float)passed)/produced);
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
        
        //cdb.printChunks(/*nextExportSize=*/sizePerExport);

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
#endif
