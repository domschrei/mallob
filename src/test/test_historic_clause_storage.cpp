
#include "app/sat/sharing/buffer/adaptive_clause_database.hpp"
#include "util/sys/process.hpp"
#include "util/sys/thread_pool.hpp"
#include "util/random.hpp"
#include "util/logger.hpp"
#include "util/sys/timer.hpp"
#include "app/sat/job/historic_clause_storage.hpp"

void testSimple() {

    AdaptiveClauseDatabase::Setup setup;
    setup.maxEffClauseLength = 20;
    setup.slotsForSumOfLengthAndLbd = false;
    setup.maxLbdPartitionedSize = 2;
    setup.numLiterals = 1500;
    setup.useChecksums = false;
    HistoricClauseStorage storage(setup);

    // for adding clauses
    AdaptiveClauseDatabase cdb(setup);
    Mallob::Clause clause;

    for (int epoch = 0; epoch <= 100; epoch++) {
        std::vector<int> clauses;
        auto builder = cdb.getBufferBuilder(&clauses);
        std::vector<int> lits((int) (2 + 6 * Random::rand()), epoch+1);
        clause.begin = lits.data();
        clause.size = lits.size();
        clause.lbd = 2;
        builder.append(clause);
        storage.addSharingAndPrepareResharing(epoch, clauses);
        while (!storage.hasPreparedResharing()); // busy waiting
        auto resharing = storage.getResharing();
        std::string litsOut;
        for (int lit : resharing.clauses) litsOut += std::to_string(lit) + " ";
        LOG(V2_INFO, "RESHARING e=%i: [%i,%i) %s\n", epoch, resharing.epochBegin, resharing.epochEnd, litsOut.c_str());
    }
}

int main() {
    Timer::init();
    Random::init(rand(), rand());
    Logger::init(0, V5_DEBG);
    Process::init(0);
    ProcessWideThreadPool::init(1);

    testSimple();
}
