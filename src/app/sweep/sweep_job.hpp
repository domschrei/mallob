
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

	typedef std::shared_ptr<Kissat> KissatPtr;

	std::vector<std::shared_ptr<Kissat>> _shweepers;
	std::vector<std::future<void>> _fut_shweepers;
    std::atomic_int _shweepers_running_count {0};

    bool _root_received_work=false;
	bool _terminate_all=false;

	// static const int NUM_STEAL_METADATA = 1;
    const int TAG_SEARCHING_WORK=1;
    const int TAG_RETURNING_STEAL_REQUEST=2;
	struct WorkstealRequest {
		int localId{-1};
		int targetRank{-1};
		bool sent{false};
		bool got_steal_response{false};
		std::vector<int> stolen_work{};
	};
	std::vector<WorkstealRequest> _worksteal_requests;


    std::unique_ptr<JobTreeBroadcast> _bcast;
    std::unique_ptr<JobTreeAllReduction> _red;
	bool _started_sharing = false;
	int _sharing_round = 0;

	//the additional metadata [..., eqs_size, units_size, all_searching_work] stored in each shared element
	static const int NUM_SHARING_METADATA = 3;
	static const int EQUIVS_SIZE_POS = 3;
	static const int UNITS_SIZE_POS = 2;
	static const int IDLE_STATUS_POS = 1;

    std::vector<int> _eqs_from_broadcast;//accumulate received equivalences to import in local solver
	std::vector<int> _units_from_broadcast;


    const int BCAST_INIT{1};
    const int ALLRED{2};

    static const int MSG_SWEEP = 100; // internal message tag
    static const int NUM_WORKERS = 4; // # workers we request and require, hardcoded 4 for now

	static const int INVALID_LIT = UINT_MAX;


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


	std::shared_ptr<Kissat> SweepJob::create_new_shweeper(int localId);
    friend void search_work_in_tree(void* SweepJob_state, unsigned **work, int *work_size, int local_id);
	// friend void import_next_equivalence(void *SweepJobState, int *last_imported_round, int eq_nr, unsigned *lit1, unsigned *lit2);

private:
    // void advanceSweepMessage(JobMessage& msg);
    static std::vector<int> aggregateContributions(std::list<std::vector<int>> &contribs);
    void loadFormula(KissatPtr shweeper);
	void startShweeper(KissatPtr shweeper);

    void tryBeginBroadcastPing();
    void callback_for_broadcast_ping();
    // void tryExtractResult();


	std::vector<int> stealWorkFromAnyLocalSolver();
    std::vector<int> stealWorkFromSpecificLocalSolver(int localId);
    void searchWorkInTree(unsigned **work, int *work_size, int localId);
	void importNextEquivalence(int *last_imported_round, int eq_nr, unsigned *lit1, unsigned *lit2);

};

#endif
