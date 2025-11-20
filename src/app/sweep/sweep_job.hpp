
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
	int _numVars{0};

	//Local Solvers
	int _nThreads{0};
	typedef std::shared_ptr<Kissat> KissatPtr;
	std::vector<KissatPtr> _sweepers;
	std::vector<std::unique_ptr<BackgroundWorker>> _bg_workers;
	// std::vector<std::future<void>> _fut_shweepers;
    std::atomic_int _running_sweepers_count {0};
	std::vector<int> _list_of_ids;

	//Timing
	float _start_sweep_timestamp;
	std::vector<float> _sharing_start_ping_timestamps;
	std::vector<float> _sharing_receive_result_timestamps;


	//Workstealing
    std::atomic_bool _root_provided_initial_work=false;
	SplitMix64Rng _rng;
	struct WorkstealRequest {
		int localId{-1};
		int targetIndex{-1};
		int targetRank{-1};
		bool sent{false};
		bool got_steal_response{false};
		std::vector<int> stolen_work{};
	};
	std::vector<WorkstealRequest> _worksteal_requests;
    const int TAG_SEARCHING_WORK = 1001;
    const int TAG_RETURNING_STEAL_REQUEST = 1002;


	//Sharing Equivalences and Units
	float _last_sharing_start_timestamp;
    std::unique_ptr<JobTreeBroadcast> _bcast;
    std::unique_ptr<JobTreeAllReduction> _red;
    const int TAG_BCAST_INIT = 1003;
    const int TAG_ALLRED = 1004;
	//Positions where the metadata is stored in each shared element. Format [ <actual data> , eqs_size, units_size, all_idle]
	static const int NUM_METADATA_FIELDS = 3; //three integers at the end of an aggregation element
	static const int METADATA_EQ_SIZE_POS = 3;
	static const int METADATA_UNIT_SIZE_POS = 2;
	static const int METADATA_IDLE_FLAG_POS = 1;

	//Distribute Eqs and Units that we received from sharing broadcast to local solvers
	// std::atomic<std::shared_ptr<std::vector<int>>> _EQS_to_import { std::make_shared<std::vector<int>>() };
	// std::atomic<std::shared_ptr<std::vector<int>>> _UNITS_to_import { std::make_shared<std::vector<int>>() };
	std::vector<int> _EQS_to_import {};
	std::vector<int> _UNITS_to_import {};
	std::vector<std::atomic_int> _unread_count_EQS_to_import {}; //a count per thread
	std::vector<std::atomic_int> _unread_count_UNITS_to_import {};
	static const unsigned INVALID_LIT = UINT_MAX; //Internal literals count unsigned 0,1,2,..., the largest number marks an invalid literal. see further: https://github.com/arminbiere/satch/blob/master/satch.c#L1017

    // std::vector<int> _eqs_from_broadcast;  //store received equivalences at rank level to copy to individual solvers
	// std::vector<int> _units_from_broadcast;


	//Termination. Determined during workstealing, broadcasted via sharing
	volatile bool _terminate_all=false; //termination (on this node) due to sharing consensus that there is no more work
	volatile bool _external_termination=false; //termination because somebody else told us to (for example Job interrupted because Base Job already found a solution, ...)


	//Keep track which solver reports the final formula, we only use one
	std::shared_ptr<std::atomic<int>> _reporting_localId = std::make_shared<std::atomic<int>>(-1);



public:
    SweepJob(const Parameters& params, const JobSetup& setup, AppMessageTable& table);
    void appl_start() override;
    void appl_communicate() override;
    void appl_communicate(int sourceRank, int mpiTag, JobMessage& msg) override;
    void appl_terminate() override;

    int appl_solved() override            {return _solved_status;}
    JobResult&& appl_getResult() override {return std::move(_internal_result);}

    void appl_suspend() override {}
    void appl_resume() override {}
    void appl_dumpStats() override {}
    bool appl_isDestructible() override {return true;}
    void appl_memoryPanic() override {}

    friend void cb_search_work_in_tree(void* SweepJob_state, unsigned **work, int *work_size, int local_id);
	friend void cb_import_eq(void *SweepJobState, int *lit1, int *lit2, int localId);
	friend void cb_import_unit(void *SweepJobState, int *lit, int localId);


private:
    // void advanceSweepMessage(JobMessage& msg);
	KissatPtr createNewSweeper(int localId);

	void createAndStartNewSweeper(int localId);
    void loadFormula(KissatPtr sweeper);

	void reportStats(KissatPtr sweeper, int res);
	// void readResult(KissatPtr shweeper, bool withStats);
	void serializeResultFormula(KissatPtr sweeper);

	void gentlyTerminateSolvers();


	void sendMPIWorkstealRequests();
	void printIdleFraction();

    void initiateNewSharingRound();
    void cbContributeToAllReduce();
    static std::vector<int> aggregateEqUnitContributions(std::list<std::vector<int>> &contribs);
	void advanceAllReduction();

	std::vector<int> getRandomIdPermutation();

	std::vector<int> stealWorkFromAnyLocalSolver();
    std::vector<int> stealWorkFromSpecificLocalSolver(int localId);
    void cbSearchWorkInTree(unsigned **work, int *work_size, int localId);
	void cbImportEq(int *ilit1, int *ilit2, int localId);
	void cbImportUnit(int *lit, int localId);
	// void importNextEquivalence(int *last_imported_round, int eq_nr, unsigned *lit1, unsigned *lit2);

	virtual ~SweepJob();

};

#endif
