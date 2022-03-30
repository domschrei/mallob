
#include "sat_process_config.hpp"
#include "app/job.hpp"
#include "util/sys/proc.hpp"

SatProcessConfig::SatProcessConfig(const Parameters& params, const Job& job, int recoveryIndex) {

    auto t = Timer::getStartTime();
    starttimeSecs = t.tv_sec;
    starttimeNsecs = t.tv_nsec;
    apprank = job.getIndex();
    mpirank = job.getMyMpiRank();
    mpisize = job.getGlobalNumWorkers();
    jobid = job.getId();
    incremental = job.getDescription().isIncremental();
    firstrev = job.getDesiredRevision();
    threads = job.getNumThreads();
    maxBroadcastedLitsPerCycle = (1+params.clauseHistoryAggregationFactor()) *
    MyMpi::getBinaryTreeBufferLimit(job.getGlobalNumWorkers(), params.clauseBufferBaseSize(), params.clauseBufferDiscountFactor(), MyMpi::ALL);
    this->recoveryIndex = recoveryIndex;
}

std::string SatProcessConfig::getSharedMemId(pid_t pid) const {
    return  "/edu.kit.iti.mallob." + std::to_string(pid) + "." 
        + std::to_string(mpirank) + ".#" + std::to_string(jobid) 
        + (recoveryIndex == 0 ? std::string() : "~" + std::to_string(recoveryIndex));
}
