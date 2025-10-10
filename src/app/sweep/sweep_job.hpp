
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
    std::atomic_int _running_shweepers_count {0};
	std::vector<int> _list_of_ids;

	float _start_shweep_timestamp;
    bool _root_provided_initial_work=false;
	bool _terminate_all=false;

	// static const int NUM_STEAL_METADATA = 1;
    const int TAG_SEARCHING_WORK=1001;
    const int TAG_RETURNING_STEAL_REQUEST=1002;
	struct WorkstealRequest {
		int localId{-1};
		int targetIndex{-1};
		int targetRank{-1};
		bool sent{false};
		bool got_steal_response{false};
		std::vector<int> stolen_work{};
	};
	std::vector<WorkstealRequest> _worksteal_requests;


	float _last_sharing_timestamp;
    std::unique_ptr<JobTreeBroadcast> _bcast;
    std::unique_ptr<JobTreeAllReduction> _red;
    const int BCAST_INIT{1};
    const int ALLRED{2};

	// bool _started_sharing = false;
	// int _sharing_round = 0;

	//Positions where the additional metadata is stored in each shared element [..., eqs_size, units_size, all_idle]
	static const int NUM_SHARING_METADATA = 3;
	static const int EQUIVS_SIZE_POS = 3;
	static const int UNITS_SIZE_POS = 2;
	static const int IDLE_STATUS_POS = 1;

    std::vector<int> _eqs_from_broadcast;  //store received equivalences to copy to individual solvers
	std::vector<int> _units_from_broadcast;

	// int _eqs_found{-1};
	// int _sweep_units_found{-1};
    // static const int MSG_SWEEP = 100; // internal message tag
    // static const int NUM_WORKERS = 4; // # workers we request and require, hardcoded 4 for now

	static const int INVALID_LIT = UINT_MAX;

	//keep track which solver reports the final formula, we need only one
	std::shared_ptr<std::atomic<int>> _dimacsReportLocalId = std::make_shared<std::atomic<int>>(-1);


public:
    SweepJob(const Parameters& params, const JobSetup& setup, AppMessageTable& table);
    void appl_start() override;
    void appl_communicate() override;
    void appl_communicate(int sourceRank, int mpiTag, JobMessage& msg) override;

    int appl_solved() override            {return _solved_status;}
    JobResult&& appl_getResult() override {return std::move(_internal_result);}

    void appl_suspend() override {}
    void appl_resume() override {}
    void appl_terminate() override {}
    void appl_dumpStats() override {}
    bool appl_isDestructible() override {return true;}
    void appl_memoryPanic() override {}


    friend void search_work_in_tree(void* SweepJob_state, unsigned **work, int *work_size, int local_id);
	// friend void import_next_equivalence(void *SweepJobState, int *last_imported_round, int eq_nr, unsigned *lit1, unsigned *lit2);

	// bool isDimacsReportStarted();
	// void setDimacsReportStarted();
	// void setDimacsReportingLocalId(int local_id);
	// std::shared_ptr<Kissat> _reporting_Kissat_obj = nullptr;


private:
    // void advanceSweepMessage(JobMessage& msg);
	KissatPtr createNewShweeper(int localId);
    void loadFormula(KissatPtr shweeper);
	void startShweeper(KissatPtr shweeper);


	void sendMPIWorkstealRequests();
    void initiateNewSharingRound();
    void contributeToAllReduceCallback();
    static std::vector<int> aggregateEqUnitContributions(std::list<std::vector<int>> &contribs);
	void advanceAllReduction();
    // void tryExtractResult();

	std::vector<int> getRandomIdPermutation();

	std::vector<int> stealWorkFromAnyLocalSolver();
    std::vector<int> stealWorkFromSpecificLocalSolver(int localId);
    void searchWorkInTree(unsigned **work, int *work_size, int localId);
	void importNextEquivalence(int *last_imported_round, int eq_nr, unsigned *lit1, unsigned *lit2);

};

#endif
