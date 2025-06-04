
#ifndef DOMPASCH_MALLOB_SWEEP_JOB_HPP
#define DOMPASCH_MALLOB_SWEEP_JOB_HPP

#include "app/job.hpp"
#include "../sat/solvers/kissat.hpp"



class SweepJob : public Job {

private:
    JobResult _result;


    int _my_rank{0};
    int _my_index{0};
    bool _is_root{false};

    uint8_t* _data;
	std::shared_ptr<Kissat> _swissat;


public:
    SweepJob(const Parameters& params, const JobSetup& setup, AppMessageTable& table);
        // : Job(params, setup, table) {}
    void appl_start() override;
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

#endif
