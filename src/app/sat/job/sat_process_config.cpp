
#include "sat_process_config.hpp"

#include <time.h>

#include "app/job.hpp"
#include "comm/binary_tree_buffer_limit.hpp"
#include "comm/mympi.hpp"
#include "data/job_description.hpp"
#include "util/logger.hpp"
#include "util/option.hpp"
#include "util/params.hpp"
#include "util/sys/timer.hpp"

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
    maxBroadcastedLitsPerCycle = params.maxSharingCompensationFactor() *
    BinaryTreeBufferLimit::getLimit(job.getGlobalNumWorkers(), params.clauseBufferBaseSize(), params.clauseBufferLimitParam(), BinaryTreeBufferLimit::BufferQueryMode(params.clauseBufferLimitMode()));
    this->recoveryIndex = recoveryIndex;
    nbPreviousBalancingEpochs = job.getLatestJobBalancingEpoch() - job.getDescription().getFirstBalancingEpoch() - 1;
    LOG(V5_DEBG, "#%i:%i : Job balancing epochs: %i (first: %i)\n", job.getId(), job.getIndex(),
        nbPreviousBalancingEpochs, job.getDescription().getFirstBalancingEpoch());
}

std::string SatProcessConfig::getSharedMemId(pid_t pid) const {
    return  "/edu.kit.iti.mallob." + std::to_string(pid) + "." 
        + std::to_string(mpirank) + ".#" + std::to_string(jobid) 
        + (recoveryIndex == 0 ? std::string() : "~" + std::to_string(recoveryIndex));
}
