
#ifndef DOMPASCH_MALLOB_DUMMY_JOB_HPP
#define DOMPASCH_MALLOB_DUMMY_JOB_HPP

#include "app/job.hpp"

class DummyJob : public Job {

public:
    DummyJob(const Parameters& params, int commSize, int worldRank, int jobId) 
        : Job(params, commSize, worldRank, jobId) {}
    void appl_start() {}
    void appl_stop() {}
    void appl_suspend() {}
    void appl_resume() {}
    void appl_interrupt() {}
    void appl_restart() {}
    void appl_terminate() {}
    int appl_solved() {return -1;}
    JobResult appl_getResult() {return JobResult();}
    bool appl_wantsToBeginCommunication() {return false;}
    void appl_beginCommunication() {}
    void appl_communicate(int source, JobMessage& msg) {}
    void appl_dumpStats() {}
    bool appl_isDestructible() {return true;}
};

#endif
