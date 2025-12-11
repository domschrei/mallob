
#ifndef DOMPASCH_MALLOB_SWEEP_JOB_HPP
#define DOMPASCH_MALLOB_SWEEP_JOB_HPP

#include <shared_mutex>

#include "app/job.hpp"
#include "../sat/solvers/kissat.hpp"
#include "comm/job_tree_all_reduction.hpp"
#include "comm/job_tree_broadcast.hpp"


// #define IMPORT_TECHNIQUE 3

class SweepJob : public Job {
private:

    JobResult _internal_result;
    int _solved_status{-1};

    int _my_rank{0};
    int _my_index{0};
    bool _is_root{false};
    uint8_t* _metadata; //serialized description
	int _numVars{0};

	int _representative_localId{0}; //the dedicated solver that reports its statistics to us. They will ever so slightly differ between solvers, but instead of doing more complicated averaging we just report this one

	//Local Solvers
	int _nThreads{0};
	typedef std::shared_ptr<Kissat> KissatPtr;
	std::vector<KissatPtr> _sweepers;
	std::vector<std::unique_ptr<BackgroundWorker>> _bg_workers;
	// std::vector<std::future<void>> _fut_shweepers;
    std::atomic_int _started_sweepers_count {0};
    std::atomic_int _running_sweepers_count {0};
	std::vector<int> _list_of_ids;
	// std::atomic_bool _finished_job_setup{false};
	bool _started_synchronized_solving{false};

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

	static const int NUM_METADATA_FIELDS = 5; //we have integers as metadata at the end of an aggregation element
		//which are:
		static const int METADATA_POS_TERMINATE_FLAG   = 5;
		static const int METADATA_POS_ROUND_FLAG      = 4;
		static const int METADATA_POS_IDLE_FLAG		 = 3;
		static const int METADATA_POS_UNIT_SIZE	    = 2;
		static const int METADATA_POS_EQ_SIZE	   = 1;


	//Distribute Eqs and Units that we received from sharing broadcast to local solvers
	static const unsigned INVALID_LIT = UINT_MAX; //Internal literals count unsigned 0,1,2,..., the largest number marks an invalid literal. see further: https://github.com/arminbiere/satch/blob/master/satch.c#L1017
	static const int MAX_IMPORT_SIZE = 100'000;
	std::atomic_int _rank_import_round{0}; //counts the number of import rounds on this rank. Polled by solvers to detect whether there is something new to import
	std::atomic_int _EQS_import_size{0};
	std::atomic_int _UNITS_import_size{0};
	std::vector<int> _EQS_to_import {};
	std::vector<int> _UNITS_to_import {};

	//We want to test multiple rounds of full sweeping, to see whether and how fast there is diminishing returns
	int _root_sweep_round = 1;

	//Termination. Determined during workstealing, broadcasted via sharing
	std::atomic_bool _terminate_all=false; //termination (on this node) due to sharing consensus that there is no more work
	std::atomic_bool _external_termination=false; //termination because somebody else told us to (for example Job interrupted because Base Job already found a solution, ...)

	//Keep track which solver reports the final formula, we only use one
	std::shared_ptr<std::atomic<int>> _reporting_localId = std::make_shared<std::atomic<int>>(-1);

	std::vector<int> _worksweeps{}; //to collect statistics
	std::vector<int> _resweeps_in{};
	std::vector<int> _resweeps_out{};

	Logger _reslogger;

	std::function<void(std::vector<int>&)> _inplace_rootTransform = [&](std::vector<int>& payload) {
		assert(_is_root);
		bool all_idle = payload[payload.size() - METADATA_POS_IDLE_FLAG];

		//A round is finished if all sweepers are idle, i.e. all finished their work.
		if (all_idle) {
			LOG(V1_WARN, "[%i] SWEEP ROUND %i/%i FINISHED (seen at root transform) \n", _my_rank, _root_sweep_round, _params.sweepRounds());
			if (_root_sweep_round == _params.sweepRounds()) {
				LOG(V1_WARN, "SWEEP [%i]: Job finished! All rounds done (%i/%i). Broadcasting termination signal with sharing data.\n", _my_rank, _root_sweep_round, _params.sweepRounds());
				payload[payload.size() - METADATA_POS_TERMINATE_FLAG] = 1;
				//we DON'T yet set _terminate_all=1 here, because we want also the root solver to first import this last sharing information, which contains valuable equalities and units, before terminating the solvers
			}
			else {
				printSweepStats(_sweepers[_representative_localId], false); //report some intermediate statistics about this round
				_root_sweep_round++;
				//The new round is started by providing the representative solver on the root node full work, i.e. all variables
				_root_provided_initial_work = false;
				LOG(V1_WARN, "[%i] SWEEP ROUND %i/%i STARTED \n", _my_rank, _root_sweep_round, _params.sweepRounds());
			}
		}
		//The root node (and only the root node) tracks the number of completed sweep rounds, and broadcasts this information. This way, also nodes that join later know which round we are in.
		payload[payload.size() - METADATA_POS_ROUND_FLAG] = _root_sweep_round;

		//no return, payload was just transformed in-place
    };

	enum CustomQuery {
		QUERY_SWEEP_ROUND = 1
	};


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
    void appl_memoryPanic() override;

    friend void cb_search_work_in_tree(void* SweepJob_state, unsigned **work, int *work_size, int local_id);
	friend void cb_import_eq(void *SweepJobState, int *lit1, int *lit2, int localId);
	friend void cb_import_unit(void *SweepJobState, int *lit, int localId);
	friend int cb_custom_query(void *SweeJobState, int query);


private:
    // void advanceSweepMessage(JobMessage& msg);
	KissatPtr createNewSweeper(int localId);

	void createAndStartNewSweeper(int localId);
    void loadFormula(KissatPtr sweeper);

	void reportSolverResult(KissatPtr sweeper, int res);
	void printSweepStats(KissatPtr sweeper, bool full);
	// void readResult(KissatPtr shweeper, bool withStats);
	// void serializeResultFormula(KissatPtr sweeper);

	void gentlyTerminateSolvers();


	void sendMPIWorkstealRequests();
	void printIdleFraction();
	void printResweeps();

    void initiateNewSharingRound();
    void cbContributeToAllReduce();
    static std::vector<int> aggregateEqUnitContributions(std::list<std::vector<int>> &contribs);
	static void appendMetadataToReductionElement(std::vector<int> &contrib, int is_idle, int unit_size, int eq_size);
	void advanceAllReduction();

	std::vector<int> getRandomIdPermutation();

	void provideInitialWork(KissatPtr sweeper);
	std::vector<int> stealWorkFromAnyLocalSolver(int asking_rank, int asking_localId); //parameters only for verbose logging
    std::vector<int> stealWorkFromSpecificLocalSolver(int localId);
    void cbSearchWorkInTree(unsigned **work, int *work_size, int localId);
	void checkForNewImportRound(KissatPtr sweeper);
	void cbImportEq(int *ilit1, int *ilit2, int localId);
	void cbImportUnit(int *lit, int localId);
	int cbCustomQuery(int query);
	// void importNextEquivalence(int *last_imported_round, int eq_nr, unsigned *lit1, unsigned *lit2);

	virtual ~SweepJob();

};

#endif
