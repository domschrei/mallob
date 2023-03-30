
#include <algorithm>

#include "app/sat/sharing/adaptive_import_manager.hpp"

#include "util/sys/process.hpp"
#include "util/sys/thread_pool.hpp"
#include "util/random.hpp"
#include "util/logger.hpp"
#include "util/sys/timer.hpp"
#include "app/sat/sharing/store/adaptive_clause_store.hpp"
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

int getProducer(const Mallob::Clause& clause, int nbProducers) {
    int sum = 0;
    for (int i = 0; i < clause.size; i++) sum += std::abs(clause.begin[i]);
    return sum % nbProducers;
}

void testImport() {

    SolverSetup setup;
    setup.strictClauseLengthLimit = 20;
	setup.strictLbdLimit = 20;
	setup.clauseBaseBufferSize = 1500;
	setup.anticipatedLitsToImportPerCycle = 200'000;
	setup.solverRevision = 0;
	setup.minNumChunksPerSolver = 100;
	setup.numBufferedClsGenerations = 4;
    SolverStatistics stats;
    stats.histProduced = new ClauseHistogram(20);
    stats.histDigested = new ClauseHistogram(20);
    AdaptiveImportManager importBuffer(setup, stats);

    // Generate some number of clauses
    std::vector<Mallob::Clause> clauses;
    for (int i = 0; i < 1000; i++) {
        clauses.push_back(generateClause(1, setup.strictClauseLengthLimit));
    }
    std::sort(clauses.begin(), clauses.end());

    // Write sorted clauses into flat buffer
    // and filter the (pretended) self-produced clauses
    // for each solver
    BufferBuilder builder(setup.anticipatedLitsToImportPerCycle, setup.strictClauseLengthLimit, false);
    int nbSolvers = 4;
    std::vector<std::vector<bool>> filters(nbSolvers);
    for (auto& clause : clauses) {
        builder.append(clause);
        int producer = getProducer(clause, filters.size());
        LOG(V2_INFO, "%s : produced by %i\n", clause.toStr().c_str(), producer);
        for (int solverId = 0; solverId < filters.size(); solverId++) {
            filters[solverId].push_back(producer == solverId);
        }
    }
    std::vector<int> flatBuffer = builder.extractBuffer();

    // Perform filtered import for each solver
    for (int solverId = 0; solverId < filters.size(); solverId++) {
        int nbAdmittedLits = 0;
        int nbAdmittedCls = 0;
        std::set<Mallob::Clause> admittedClauses;
        // Verify output of buffer reader with respective filter set
        {
            BufferReader reader(flatBuffer.data(), flatBuffer.size(), setup.strictClauseLengthLimit, false);
            reader.setFilterBitset(filters[solverId]);
            auto clause = reader.getNextIncomingClause();
            while (clause.begin != nullptr) {
                assert(getProducer(clause, filters.size()) != solverId);
                nbAdmittedCls++;
                nbAdmittedLits += clause.size;
                admittedClauses.insert(clause);
                clause = reader.getNextIncomingClause();
            }
            LOG(V2_INFO, "solver #%i : %i/%i admitted (%i lits)\n", solverId, nbAdmittedCls, clauses.size(), nbAdmittedLits);
        }
        // Import
        {
            BufferReader reader(flatBuffer.data(), flatBuffer.size(), setup.strictClauseLengthLimit, false);
            reader.setFilterBitset(filters[solverId]);
            importBuffer.performImport(reader);
            LOG(V2_INFO, "import buffer now has size %i\n", importBuffer.size());
            assert(importBuffer.size() == nbAdmittedLits);
        }
        // Retrieval of unit clauses
        int nbRetrievedCls = 0;
        {
            auto units = importBuffer.getUnitsBuffer();
            for (int unit : units) {
                Mallob::Clause clause(&unit, 1, 1);
                assert(getProducer(clause, filters.size()) != solverId);
                assert(admittedClauses.count(clause));
                admittedClauses.erase(clause);
                nbRetrievedCls++;
            }
            LOG(V2_INFO, "Retrieved %i units\n", units.size());
        }
        // Retrieval of non-unit clauses
        {
            auto clause = importBuffer.get(AdaptiveClauseStore::NONUNITS);
            int nbRetrievedNonunits = 0;
            while (!importBuffer.empty() || clause.begin != nullptr) {
                if (clause.begin != nullptr) {
                    assert(getProducer(clause, filters.size()) != solverId);
                    assert(admittedClauses.count(clause));
                    admittedClauses.erase(clause);
                    nbRetrievedCls++;
                    nbRetrievedNonunits++;
                }
                //LOG(V2_INFO, "%lu lits remaining; pop result: %s\n", importBuffer.size(), clause.begin==nullptr ? "(null)" : clause.toStr().c_str());
                clause = importBuffer.get(AdaptiveClauseStore::NONUNITS);
            }
            LOG(V2_INFO, "Retrieved %i non-units\n", nbRetrievedNonunits);
        }
        assert(importBuffer.empty());
        if (!admittedClauses.empty()) {
            for (auto& clause : admittedClauses) {
                log_return_false("[ERROR] Admitted clause could not be retrieved: %s\n", clause.toStr().c_str());
            }
            assert(false);
        }
        assert(nbRetrievedCls == nbAdmittedCls || 
            log_return_false("[ERROR] Mismatch in admitted vs. retrieved clauses (%i vs. %i)\n", nbAdmittedCls, nbRetrievedCls));
    }        
}


int main() {
    Timer::init();
    Random::init(rand(), rand());
    Logger::init(0, V5_DEBG);
    Process::init(0);
    ProcessWideThreadPool::init(4);
    
    testImport();
}
