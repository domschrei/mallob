
#ifndef DOMPASCH_MALLOB_KMEANS_JOB_HPP
#define DOMPASCH_MALLOB_KMEANS_JOB_HPP

#include "app/job.hpp"

/*
Minimally compiling example "application" for a Mallob job. 
Edit and extend for your application. 
*/
class KMeansJob : public Job {

public:
    KMeansJob(const Parameters& params, int commSize, int worldRank, int jobId) 
        : Job(params, commSize, worldRank, jobId, JobDescription::Application::KMEANS) {}
    void appl_start() override {}
    void appl_suspend() override {}
    void appl_resume() override {}
    void appl_terminate() override {}
    int appl_solved() override {return -1;}
    JobResult&& appl_getResult() override {return JobResult();}
    void appl_communicate() override {}
    void appl_communicate(int source, int mpiTag, JobMessage& msg) override {}
    void appl_dumpStats() override {}
    bool appl_isDestructible() override {return true;}
};

#endif
