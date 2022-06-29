
#include <algorithm>

#include "app/sat/sharing/import_buffer.hpp"

#include "util/sys/process.hpp"
#include "util/sys/thread_pool.hpp"
#include "util/random.hpp"
#include "util/logger.hpp"
#include "util/sys/timer.hpp"
#include "app/sat/sharing/buffer/adaptive_clause_database.hpp"
#include "app/sat/sharing/buffer/buffer_merger.hpp"
#include "app/sat/sharing/buffer/buffer_reducer.hpp"
#include "util/sys/terminator.hpp"
#include "util/assert.hpp"

Mallob::Clause generateClause(int minLength, int maxLength) {
    int length = minLength + (int) (Random::rand() * (maxLength-minLength));
    assert(length >= minLength);
    assert(length <= maxLength);
    int lbd = 2 + (int) (Random::rand() * (length-2));
    if (length == 1) lbd = 1;
    assert(lbd >= 2 || (length == 1 && lbd == 1));
    assert(lbd <= length);
    Mallob::Clause c((int*)malloc(length*sizeof(int)), length, lbd);
    for (size_t i = 0; i < length; ++i) {
        c.begin[i] = -100000 + (int) (Random::rand() * 200001);
        assert(c.begin[i] >= -100000);
        assert(c.begin[i] <= 100000);
    }
    std::sort(c.begin, c.begin+length);
    return c;
}

void testConcurrentImport() {

    SolverSetup setup;
    setup.strictClauseLengthLimit = 20;
	setup.strictLbdLimit = 20;
	setup.clauseBaseBufferSize = 1500;
	setup.anticipatedLitsToImportPerCycle = 20000;
	setup.solverRevision = 0;
	setup.minNumChunksPerSolver = 100;
	setup.numBufferedClsGenerations = 4;
    SolverStatistics stats;
    stats.histProduced = new ClauseHistogram(20);
    stats.histDigested = new ClauseHistogram(20);
    ImportBuffer importBuffer(setup, stats);
    LOG(V2_INFO, "setup\n");
    
    int nbTotalAdded = 0;
    int nbTotalDigested = 0;

    // Producer
    auto futureProd = ProcessWideThreadPool::get().addTask([&]() {
        std::forward_list<int> units;
        std::forward_list<std::pair<int, int>> binaries;
        std::map<std::pair<int, int>, std::forward_list<std::vector<int>>> lenLbdToList;

        float startTime = Timer::elapsedSeconds();
        float lastImport = startTime;

        auto pushClauses = [&]() {
            LOG(V2_INFO, "Adding clauses to import buffer\n");
            int budget, remaining, nbAdded = 0;
            
            budget = importBuffer.getLiteralBudget(1, 1); remaining = budget;
            units.remove_if([&](int i) {
                if (remaining == 0) return true;
                remaining--;
                nbAdded++;
                return false;
            });
            importBuffer.performImport(1, 1, units, budget-remaining);

            budget = importBuffer.getLiteralBudget(2, 2); remaining = budget;
            binaries.remove_if([&](const auto& pair) {
                if (remaining < 2) return true;
                remaining -= 2;
                nbAdded++;
                return false;
            });
            importBuffer.performImport(2, 2, binaries, budget-remaining);

            for (auto& entry : lenLbdToList) {
                auto [len, lbd] = entry.first;
                auto& list = entry.second;
                budget = importBuffer.getLiteralBudget(len, lbd); remaining = budget;
                list.remove_if([&](const auto& pair) {
                    if (remaining < len) return true;
                    remaining -= len;
                    nbAdded++;
                    return false;
                });
                importBuffer.performImport(len, lbd, list, budget-remaining);
            }

            lastImport = Timer::elapsedSeconds();
            LOG(V2_INFO, "Added %i clauses to import buffer\n", nbAdded);
        };

        while (Timer::elapsedSeconds() - startTime <= 60 && !Terminator::isTerminating()) {

            auto cls = generateClause(1, setup.strictClauseLengthLimit);
            if (cls.size == 1) {
                units.push_front(cls.begin[0]);
            } else if (cls.size == 2) {
                binaries.emplace_front(cls.begin[0], cls.begin[1]);
            } else {
                std::vector<int> clauseVec(1+cls.size);
                clauseVec[0] = cls.lbd;
                for (size_t k = 0; k < cls.size; k++) clauseVec[k+1] = cls.begin[k];
                lenLbdToList[std::pair<int, int>(cls.size, cls.lbd)].emplace_front(std::move(clauseVec));
            }
            nbTotalAdded++;
            usleep(1000 * 1); // 1 millis

            if (Timer::elapsedSeconds() - lastImport >= 1) {
                pushClauses();
            }
        }
        pushClauses();
    });

    auto futureCons = ProcessWideThreadPool::get().addTask([&]() {
        float startTime = Timer::elapsedSeconds();
        while ((Timer::elapsedSeconds() - startTime <= 60 && !Terminator::isTerminating()) || !importBuffer.empty()) {
            auto units = importBuffer.getUnitsBuffer();
            if (!units.empty()) LOG(V2_INFO, "Received %i units\n", units.size());
            nbTotalDigested += units.size();
            auto cls = importBuffer.get(AdaptiveClauseDatabase::NONUNITS);
            while (cls.begin != nullptr) {
                LOG(V2_INFO, "Received %s\n", cls.toStr().c_str());
                nbTotalDigested++;
                cls = importBuffer.get(AdaptiveClauseDatabase::NONUNITS);
            }
            usleep(1000 * 10); // 10 millis
        }
    });

    futureProd.get();
    futureCons.get();

    LOG(V2_INFO, "%i produced, %i digested\n", nbTotalAdded, nbTotalDigested);
}

int main() {
    Timer::init();
    Random::init(rand(), rand());
    Logger::init(0, V5_DEBG);
    Process::init(0);
    ProcessWideThreadPool::init(4);
    
    testConcurrentImport();
}
