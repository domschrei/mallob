
#ifndef DOMPASCH_MALLOB_DUMMY_JOB_HPP
#define DOMPASCH_MALLOB_DUMMY_JOB_HPP

#include "app/job.hpp"

/*
Minimally compiling example "application" for a Mallob job. 
Edit and extend for your application. 
*/
class DummyJob : public Job {

public:
    DummyJob(const Parameters& params, int commSize, int worldRank, int jobId) 
        : Job(params, commSize, worldRank, jobId, JobDescription::Application::DUMMY) {}
    void appl_start() override {}
    void appl_suspend() override {}
    void appl_resume() override {}
    void appl_terminate() override {}
    int appl_solved() override {return -1;}
    JobResult&& appl_getResult() override {return JobResult();}
    bool appl_wantsToBeginCommunication() override {return false;}
    void appl_beginCommunication() override {}
    void appl_communicate(int source, JobMessage& msg) override {}
    void appl_dumpStats() override {}
    bool appl_isDestructible() override {return true;}
};

#endif
