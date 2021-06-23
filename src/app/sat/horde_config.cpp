
#include "horde_config.hpp"
#include "app/job.hpp"

HordeConfig::HordeConfig(const Parameters& params, const Job& job) {
    starttime = Timer::elapsedSeconds();
    apprank = job.getIndex();
    mpirank = job.getMyMpiRank();
    mpisize = job.getGlobalNumWorkers();
    jobid = job.getId();
    incremental = job.getDescription().isIncremental();
    firstrev = job.getRevision();
    threads = job.getNumThreads();
    maxBroadcastedLitsPerCycle = (1+params.clauseHistoryAggregationFactor()) *
    MyMpi::getBinaryTreeBufferLimit(job.getGlobalNumWorkers(), params.clauseBufferBaseSize(), params.clauseBufferDiscountFactor(), MyMpi::ALL);
}
