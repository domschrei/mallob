
#ifndef DOMPASCH_MALLOB_SWEEP_JOB_HPP
#define DOMPASCH_MALLOB_SWEEP_JOB_HPP

#include "app/job.hpp"
#include "../sat/solvers/kissat.hpp"



class SweepJob : public Job {
private:
    JobResult _internal_result;
    int _solved_status{-1};

    int _my_rank{0};
    int _my_index{0};
    bool _is_root{false};
    uint8_t* _metadata; //serialized description

    std::shared_ptr<Kissat> _swissat;
    std::future<void> _fut_swissat;
    std::atomic_int _swissat_running {0};


    static const int MSG_SWEEP = 100; // internal message tag
    static const int NUM_WORKERS = 4; // # workers we request and require, hardcoded 4 for now


public:
    SweepJob(const Parameters& params, const JobSetup& setup, AppMessageTable& table);
    void appl_start() override;
    void appl_communicate() override;
    void appl_communicate(int source, int mpiTag, JobMessage& msg) override;

    int appl_solved() override            {return _solved_status;}
    JobResult&& appl_getResult() override {return std::move(_internal_result);}

    void appl_suspend() override {}
    void appl_resume() override {}
    void appl_terminate() override {}
    void appl_dumpStats() override {}
    bool appl_isDestructible() override {return true;}
    void appl_memoryPanic() override {}

private:
    void advanceSweepMessage(JobMessage& msg);
    void loadFormulaToSwissat();

};

#endif
