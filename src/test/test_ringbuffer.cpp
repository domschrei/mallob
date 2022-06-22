
#include <iostream>
#include "util/assert.hpp"
#include <vector>
#include <string>
#include <thread>
#include <set>
#include <atomic>

#include "util/random.hpp"
#include "util/sat_reader.hpp"
#include "util/logger.hpp"
#include "util/sys/timer.hpp"
#include "util/ringbuffer.hpp"
#include "app/sat/data/clause_ringbuffer.hpp"

std::atomic_int numDefaultConstructed = 0;
std::atomic_int numCopyConstructed = 0;
std::atomic_int numMoveConstructed = 0;
std::atomic_int numCopyAssigned = 0;
std::atomic_int numMoveAssigned = 0;

struct DataObj {
    int a;
    float b;
    std::vector<int> c;

    DataObj() {
        numDefaultConstructed++;
    }
    DataObj(DataObj&& other): a(other.a), b(other.b) {
        c = std::move(other.c);
        numMoveConstructed++;
    }
    DataObj(const DataObj& other) {
        numCopyConstructed++;
        if (!other.c.empty()) abort();
        a = other.a;
        b = other.b;
        c = other.c;
    }

    DataObj& operator=(DataObj&& other) {
        a = other.a;
        b = other.b;
        c = std::move(other.c);
        numMoveAssigned++;
        return *this;
    }
    DataObj& operator=(const DataObj& other) {
        a = other.a;
        b = other.b;
        c = other.c;
        numCopyAssigned++;
        return *this;
    }
};

void testSPSCRingBuffer() {
    const int bufferSize = 128;
    const int numInsertions = 10000;

    LOG(V2_INFO, "Testing spsc ringbuffer ...\n");
    SPSCRingBuffer<DataObj> r(bufferSize);
    std::thread producer([&r]() {
        int numInserted = 0;
        DataObj o;
        while (numInserted < numInsertions) {
            o.a = 1;
            o.b = 1.5;
            o.c.resize(numInserted);
            if (r.produce(std::move(o))) {
                numInserted++;
                LOG(V2_INFO, "Prod.: %i\n", numInserted);
            }
        }
        LOG(V2_INFO, "Prod.: Done!\n");
    });

    // Consume
    int numConsumed = 0;
    while (numConsumed < numInsertions) {
        auto opt = r.consume();
        if (opt.has_value()) {
            auto& o = opt.value();
            assert(o.c.size() == numConsumed
                || LOG_RETURN_FALSE("%i != %i!\n", o.c.size(), numConsumed));
            numConsumed++;
        }
    }
    LOG(V2_INFO, "Cons.: Done!\n");

    producer.join();

    LOG(V2_INFO, "default constructed: %i\n", (int)numDefaultConstructed);
    LOG(V2_INFO, "copy constructed: %i\n", (int)numCopyConstructed);
    LOG(V2_INFO, "move constructed: %i\n", (int)numMoveConstructed);
    LOG(V2_INFO, "copy assigned: %i\n", (int)numCopyAssigned);
    LOG(V2_INFO, "move assigned: %i\n", (int)numMoveAssigned);
}

#ifdef COMMENTED_OUT
void testMixedRingbuffer() {
    LOG(V2_INFO, "Testing mixed non-unit ringbuffer ...\n");

    MixedNonunitClauseRingBuffer r(20, 3);

    std::thread producer1([&r]() {
        int v[] = {1, 2, -3}; Clause c{v, 3, /*lbd=*/2};
        int numInserted = 0;
        while (numInserted < 300) {
            if (r.insertClause(c, 0)) numInserted++;
        }
        LOG(V2_INFO, "Prod. 1: Done!\n");
    });
    std::thread producer2([&r]() {
        int v[] = {-4, -5}; Clause c{v, 2, /*lbd=*/2};
        int numInserted = 0;
        while (numInserted < 300) {
            if (r.insertClause(c, 1)) numInserted++;
        }
        LOG(V2_INFO, "Prod. 2: Done!\n");
    });
    std::thread producer3([&r]() {
        int v[] = {-6, 7, -8, 9}; Clause c{v, 4, /*lbd=*/4};
        int numInserted = 0;
        while (numInserted < 300) {
            if (r.insertClause(c, 2)) numInserted++;
        }
        LOG(V2_INFO, "Prod. 3: Done!\n");
    });

    // Consume clauses
    std::vector<int> clauses;
    int expectedSize = 300*5+300*4+300*6;
    while (clauses.size() < expectedSize) {
        bool success = r.getClause(clauses);
        if (success) LOG(V3_VERB, "Read %i lits\n", clauses.size());
    }
    assert(clauses.size() == expectedSize);

    producer1.join();
    producer2.join();
    producer3.join();

    for (int lit : clauses) LOG_OMIT_PREFIX(V4_VVER, "%i ", lit);
    LOG_OMIT_PREFIX(V4_VVER, "\n");
}
#endif

void testUnitBuffer() {
    LOG(V2_INFO, "Testing unit ringbuffer ...\n");
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
            LOG(V3_VERB, "Got %i units\n", numbers.size());
        }
    }

    std::vector<int> buf;
    assert(!r.getUnits(buf));

    for (auto& thread : threads) thread.join();
}

void testUniformClauseBuffer() {
    LOG(V2_INFO, "Testing uniform clause ringbuffer ...\n");

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
            LOG(V3_VERB, "Got %i lits\n", out.size());
        }
    }
    assert(!r.getClause(out));

    for (auto& thread : threads) thread.join();

    for (int lit : out) LOG_OMIT_PREFIX(V4_VVER, "%i ", lit);
    LOG_OMIT_PREFIX(V4_VVER, "\n");
}

void testUniformSizeClauseBuffer() {
    LOG(V2_INFO, "Testing uniform size clause ringbuffer ...\n");

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
            LOG(V3_VERB, "Got %i lits\n", out.size());
        } else assert(buf.empty());
    }
    assert(!r.getClause(out));

    for (auto& thread : threads) thread.join();

    for (int lit : out) LOG_OMIT_PREFIX(V4_VVER, "%i ", lit);
    LOG_OMIT_PREFIX(V4_VVER, "\n");
}


void testRingBufferV2() {

    {
        RingBufferV2 rb(/*size=*/1500, /*sizePerElem=*/3, /*numProducers=*/1);
        bool success;
        
        std::vector<int> data = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
        success = rb.produce(data.data()+0, /*prodId=*/0); assert(success);
        success = rb.produce(data.data()+3, /*prodId=*/0); assert(success);
        success = rb.produce(data.data()+6, /*prodId=*/0); assert(success);
        success = rb.produce(data.data()+9, /*prodId=*/0); assert(success);

        std::vector<int> out; 
        success = rb.consume(out); assert(success);
        for (int lit : out) LOG(V2_INFO, "%i\n", lit);
        success = rb.consume(out); assert(success);
        for (int lit : out) LOG(V2_INFO, "%i\n", lit);
        success = rb.consume(out); assert(success);
        for (int lit : out) LOG(V2_INFO, "%i\n", lit);
        success = rb.consume(out); assert(success);
        for (int lit : out) LOG(V2_INFO, "%i\n", lit);
        success = rb.consume(out); assert(!success);
        for (int lit : out) LOG(V2_INFO, "%i\n", lit);

        assert(out == data);
    }

    {
        RingBufferV2 rb(/*size=*/1500);
        bool success;
        
        // Assume that we insert clauses with the following size-LBD combinations:
        // (8,2) (7,3) (6,4) (5,5)
        std::vector<int> vec1 = {1, 2, 3, 4, 5};
        std::vector<int> vec2 = {1, 2, 3, 4, 5, 6};
        std::vector<int> vec3 = {1, 2, 3, 4, 5, 6, 7};
        std::vector<int> vec4 = {1, 2, 3, 4, 5, 6, 7, 8};
        success = rb.produce(vec1.data(), /*prodId=*/0, /*numIntegers=*/vec1.size(), /*headerByte=*/5); assert(success);
        success = rb.produce(vec2.data(), /*prodId=*/0, /*numIntegers=*/vec2.size(), /*headerByte=*/4); assert(success);
        success = rb.produce(vec3.data(), /*prodId=*/0, /*numIntegers=*/vec3.size(), /*headerByte=*/3); assert(success);
        success = rb.produce(vec4.data(), /*prodId=*/0, /*numIntegers=*/vec4.size(), /*headerByte=*/2); assert(success);

        std::vector<int> out;
        while (true) {
            uint8_t header;
            success = rb.getNextHeaderByte(header);
            if (!success) break;
            success = rb.consume(10-header, out);
            assert(success);
        }
        for (int lit : out) LOG(V2_INFO, "%i\n", lit);
    }
}

int main() {
    Timer::init();
    Random::init(rand(), rand());
    Logger::init(0, V5_DEBG);
    
    //testMixedRingbuffer();
    testRingBufferV2();
    exit(0);

    testUnitBuffer();
    testUniformClauseBuffer();
    testUniformSizeClauseBuffer();
    testSPSCRingBuffer();
}
