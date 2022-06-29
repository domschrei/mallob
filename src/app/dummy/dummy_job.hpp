
#ifndef DOMPASCH_MALLOB_DUMMY_JOB_HPP
#define DOMPASCH_MALLOB_DUMMY_JOB_HPP

#include "app/job.hpp"

/*
Minimally compiling example "application" for a Mallob job. 
Edit and extend for your application. 
*/
class DummyJob : public Job {

public:
    DummyJob(const Parameters& params, const JobSetup& setup) 
        : Job(params, setup) {}
    void appl_start() override {}
    void appl_suspend() override {}
    void appl_resume() override {}
    void appl_terminate() override {}
    void appl_loop() override {
        // If a result has been found for a certain revision (0 if non-incremental):
        // define a resultCode (10: satisfiable/solvable problem solved, 20: problem 
        // found unsatisfiable/unsolvable, 0: unknown result / error) and a solution.
        //publishResult(revision, resultCode, std::move(solution));
    }
    void appl_communicate() override {}
    void appl_communicate(int source, int mpiTag, JobMessage& msg) override {}
    void appl_dumpStats() override {}
    bool appl_isDestructible() override {return true;}
    void appl_memoryPanic() override {}
};

#endif
