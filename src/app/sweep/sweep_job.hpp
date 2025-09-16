
#ifndef DOMPASCH_MALLOB_SWEEP_JOB_HPP
#define DOMPASCH_MALLOB_SWEEP_JOB_HPP

#include "app/job.hpp"
#include "../sat/solvers/kissat.hpp"
#include "comm/job_tree_all_reduction.hpp"
#include "comm/job_tree_broadcast.hpp"


class SweepJob : public Job {
private:
    JobResult _internal_result;
    int _solved_status{-1};

    int _my_rank{0};
    int _my_index{0};
    bool _is_root{false};
    uint8_t* _metadata; //serialized description

    std::shared_ptr<Kissat> _shweeper;
    std::future<void> _fut_shweeper;
    std::atomic_int _shweepers_running_count {0};

    bool _is_searching_work=false;
    // const int SHWEEP_STATE_WORKING{0};
    // const int SHWEEP_STATE_IDLE{1};

    const int TAG_SEARCHING_WORK=1;
    const int TAG_SUCCESSFUL_WORK_STEAL=2;
    const int TAG_UNSUCCESSFUL_WORK_STEAL=3;

    bool root_received_work=false;
    bool got_steal_response=false;
    // std::vector<unsigned> stolen_work;


    std::unique_ptr<JobTreeBroadcast> _bcast;
    std::unique_ptr<JobTreeAllReduction> _red;
	bool started_sharing = false;

    const int BCAST_INIT{1};
    const int ALLRED{2};

    static const int MSG_SWEEP = 100; // internal message tag
    static const int NUM_WORKERS = 4; // # workers we request and require, hardcoded 4 for now

	//the additional metadata [..., eqs_size, units_size, all_searching_work] stored in each shared element
	static const int NUM_SHARING_METADATA = 3;
	static const int EQS_SIZE_POS = 3;
	static const int UNITS_SIZE_POS = 2;
	static const int SEARCH_STATUS_POS = 1;


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

    friend void search_work_in_tree(void* SweepJob_state, unsigned **work, int *work_size);

private:
    void advanceSweepMessage(JobMessage& msg);
    static std::vector<int> aggregateContributions(std::list<std::vector<int>> &contribs);
    void loadFormulaToShweeper();


    void tryBeginBroadcastPing();
    void callback_for_broadcast_ping();
    void tryExtractResult();

    int steal_from_my_local_solver();
    void searchWorkInTree(unsigned **work, int *work_size);

};

#endif
