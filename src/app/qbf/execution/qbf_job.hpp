
#pragma once

#include "app/job.hpp"

/*
Minimally compiling example "application" for a Mallob job. 
Edit and extend for your application. 
*/
class QbfJob : public Job {

private:
    JobResult _result;

public:
    QbfJob(const Parameters& params, const JobSetup& setup, AppMessageTable& table) 
        : Job(params, setup, table) {}
    void appl_start() override {}
    void appl_suspend() override {}
    void appl_resume() override {}
    void appl_terminate() override {}
    int appl_solved() override {return -1;}
    JobResult&& appl_getResult() override {return std::move(_result);}
    void appl_communicate() override {}
    void appl_communicate(int source, int mpiTag, JobMessage& msg) override {}
    void appl_dumpStats() override {}
    bool appl_isDestructible() override {return true;}
    void appl_memoryPanic() override {}
};
