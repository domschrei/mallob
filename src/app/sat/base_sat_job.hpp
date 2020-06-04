
#ifndef DOMPASCH_MALLOB_BASE_SAT_JOB_H
#define DOMPASCH_MALLOB_BASE_SAT_JOB_H

#include "app/job.hpp"

class BaseSatJob : public Job {

public:
    BaseSatJob(Parameters& params, int commSize, int worldRank, int jobId, EpochCounter& epochCounter) : 
        Job(params, commSize, worldRank, jobId, epochCounter) {}
    virtual ~BaseSatJob() {}

    // Methods common to all BaseSatJob instances

    virtual bool isInitialized() = 0;
    virtual void prepareSharing(int maxSize) = 0;
    virtual bool hasPreparedSharing() = 0;
    virtual std::vector<int> getPreparedClauses() = 0;
    virtual void digestSharing(const std::vector<int>& clauses) = 0;

    // Methods common to all Job instances

    virtual bool appl_initialize() = 0;
    virtual bool appl_doneInitializing() = 0;
    virtual void appl_updateRole() = 0;
    virtual void appl_updateDescription(int fromRevision) = 0;
    virtual void appl_pause() = 0;
    virtual void appl_unpause() = 0;
    virtual void appl_interrupt() = 0;
    virtual void appl_withdraw() = 0;
    virtual int appl_solveLoop() = 0;
    virtual bool appl_wantsToBeginCommunication() const = 0;
    virtual void appl_beginCommunication() = 0;
    virtual void appl_communicate(int source, JobMessage& msg) = 0;
    virtual void appl_dumpStats() = 0;
    virtual bool appl_isDestructible() = 0;
};

#endif