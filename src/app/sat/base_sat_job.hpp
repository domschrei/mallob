
#ifndef DOMPASCH_MALLOB_BASE_SAT_JOB_H
#define DOMPASCH_MALLOB_BASE_SAT_JOB_H

#include "app/job.hpp"
#include "data/checksum.hpp"

class BaseSatJob : public Job {

public:
    BaseSatJob(const Parameters& params, int commSize, int worldRank, int jobId, JobDescription::Application appl) : 
        Job(params, commSize, worldRank, jobId, appl) {}
    virtual ~BaseSatJob() {}

    // Methods common to all BaseSatJob instances

    virtual bool isInitialized() = 0;
    
    virtual void prepareSharing(int maxSize) = 0;
    virtual bool hasPreparedSharing() = 0;
    virtual std::vector<int> getPreparedClauses(Checksum& checksum) = 0;
    virtual void resetLastCommTime() = 0;

    virtual void digestSharing(std::vector<int>& clauses, const Checksum& checksum) = 0;
    virtual void returnClauses(std::vector<int>& clauses) = 0;

    // Methods common to all Job instances

    virtual void appl_start() = 0;
    virtual void appl_suspend() = 0;
    virtual void appl_resume() = 0;
    virtual void appl_terminate() = 0;

    virtual int appl_solved() = 0;
    virtual JobResult&& appl_getResult() = 0;
    
    virtual bool appl_wantsToBeginCommunication() = 0;
    virtual void appl_beginCommunication() = 0;
    virtual void appl_communicate(int source, JobMessage& msg) = 0;
    
    virtual void appl_dumpStats() = 0;
    virtual bool appl_isDestructible() = 0;
};

#endif