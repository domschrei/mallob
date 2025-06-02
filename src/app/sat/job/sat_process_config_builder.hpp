
#pragma once

#include "app/job.hpp"
#include "app/sat/job/sat_process_config.hpp"
#include "comm/binary_tree_buffer_limit.hpp"
#include "util/logger.hpp"
#include "util/sys/timer.hpp"

class SatProcessConfigBuilder {

public:
    static SatProcessConfig get(const Parameters& params, const Job& job, int recoveryIndex) {
        SatProcessConfig conf;
        auto t = Timer::getStartTime();
        conf.starttimeSecs = t.tv_sec;
        conf.starttimeNsecs = t.tv_nsec;
        conf.apprank = job.getIndex();
        conf.mpirank = job.getMyMpiRank();
        conf.mpisize = job.getGlobalNumWorkers();
        conf.jobid = job.getId();
        conf.incremental = job.getDescription().isIncremental();
        conf.firstrev = job.getDesiredRevision();
        conf.threads = job.getNumThreads();
        conf.maxBroadcastedLitsPerCycle = params.maxSharingCompensationFactor() *
        BinaryTreeBufferLimit::getLimit(job.getGlobalNumWorkers(), params.clauseBufferBaseSize(), params.clauseBufferLimitParam(), BinaryTreeBufferLimit::BufferQueryMode(params.clauseBufferLimitMode()));
        conf.recoveryIndex = recoveryIndex;
        conf.nbPreviousBalancingEpochs = job.getLatestJobBalancingEpoch() - job.getDescription().getFirstBalancingEpoch() - 1;
        LOG(V5_DEBG, "#%i:%i : Job balancing epochs: %i (first: %i)\n", job.getId(), job.getIndex(),
            nbPreviousBalancingEpochs, job.getDescription().getFirstBalancingEpoch());
        return conf;
    }
};
