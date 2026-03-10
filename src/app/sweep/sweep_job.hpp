
#ifndef DOMPASCH_MALLOB_SWEEP_JOB_HPP
#define DOMPASCH_MALLOB_SWEEP_JOB_HPP

#include <shared_mutex>
#include <deque>

#include "app/job.hpp"
#include "../sat/solvers/kissat.hpp"
#include "comm/job_tree_all_reduction.hpp"
#include "comm/job_tree_broadcast.hpp"


// #define IMPORT_TECHNIQUE 3

class SweepJob : public Job {
private:

    JobResult _internal_result;
    int _solved_status{-1};
	bool _do_report_UNSAT_to_root{false};
	std::atomic_bool _root_reported_unsat{false};
	bool _finished_job_setup{false};
	bool _started_communication{false};

	bool _started_appl_start{false};
    int _my_rank{0};
    int _my_index{0};
	int _my_ctx_id{0};
    bool _is_root{false};
    uint8_t* _metadata; //serialized description
	int _numVars{0};


	const int _representative_localId{0}; //a dedicated solver that reports its statistics to us
	const int _congruence_localId{1};

	//Local Solvers
	int _nThreads{0};
	typedef std::shared_ptr<Kissat> KissatPtr;
	std::vector<KissatPtr> _sweepers;
	std::vector<std::unique_ptr<BackgroundWorker>> _bg_workers;
    std::atomic_int _started_sweepers_count {0}; //no. of initialized Kissat solvers with loaded formula. Monotonically 0..24
    std::atomic_int _running_sweepers_count {0};
	std::atomic_int _finished_sweepers_count {0};
	std::vector<int> _list_of_ids;
	std::atomic_bool _started_synchronized_solving{false};
	int _lastIdleCount{0};

	//Timing
	float _start_sweep_timestamp;
	std::vector<float> _time_start_bcast;
	std::vector<float> _time_receive_allred;
	std::vector<float> _time_contributed;

	//Workstealing
	SplitMix64Rng _rng;
    std::atomic_bool _root_provided_initial_work=false;
	struct WorkstealRequest {
		int senderLocalId{-1};
		int targetIndex{-1};
		int targetRank{-1};
		bool sent{false};
		std::atomic_bool got_steal_response{false};
		std::vector<int> stolen_work{};

		void newBlankRequest(int _senderLocalId) noexcept {
				senderLocalId = _senderLocalId;
				targetIndex = -1;
				targetRank = -1;
				sent = false;
				got_steal_response = false;
				stolen_work.clear();
		}
	};
	std::deque<WorkstealRequest> _worksteal_requests; //deque, because they have an atomic member and are thus not copyable
	const int MIN_STEAL_AMOUNT = 10; //avoid to much overhead at the very end when there is almost no work left, avoid sending around ridiculously small work packages


	//Sharing Equivalences and Units
	float _root_last_sharing_start_timestamp;
    std::unique_ptr<JobTreeBroadcast> _bcast;
    std::unique_ptr<JobTreeAllReduction> _red;

	//Sanity checks, Warn if periods get too large
	float _last_received_sharing_time{0};
	float _last_contribution_time{0};
	float _last_sharedelay_warning{0};

    const int TAG_SEARCHING_WORK= 1001;
    const int TAG_RETURNING_STEAL_REQUEST = 1002;
    const int TAG_BCAST_INIT	= 1003;
    const int TAG_ALLRED		= 1004;
	const int TAG_FOUND_UNSAT	= 1005;

	const int NUM_SEARCHING_WORK_FIELDS = 3; //must match with the number of fields we actually provide at that point (follow symbol)

	//each aggregation element has some metadata integers at the end
	static const int NUM_METADATA_FIELDS = 6;
		//field indices must be unique numbers exactly filling 1..NUM_METADATA_FIELDS !
		static const int METADATA_TERMINATE			= 6;
		static const int METADATA_SWEEP_ITERATION = 5;
		static const int METADATA_SHARING_ROUND = 4;
		static const int METADATA_IDLE		  = 3;
		static const int METADATA_UNIT_SIZE	 = 2;
		static const int METADATA_EQ_SIZE   = 1;


	//Distribute Eqs and Units that we received from sharing broadcast to local solvers
	static const unsigned INVALID_LIT = UINT_MAX; //Internal literals count unsigned 0,1,2,..., the largest number marks an invalid literal. see further: https://github.com/arminbiere/satch/blob/master/satch.c#L1017
	static const int MAX_IMPORT_SIZE = 400'000; //Limiting the import to a known preallocated area, to simplify concurrent reads and writes (still neglectable with ~ 1.6 MB)
	std::atomic_int _available_import_round{0}; //identifier for the newest import round that we received from the sharing operation
	std::atomic_int _EQS_import_size{0};
	std::atomic_int _UNITS_import_size{0};
	std::vector<int> _EQS_to_import {};
	std::vector<int> _UNITS_to_import {};


	//Termination. Determined during workstealing, broadcasted via sharing
	std::atomic_bool _terminate_all=false; //termination (on this node) due to sharing consensus that there is no more work
	std::atomic_bool _external_termination=false; //termination because somebody else told us to (for example Job interrupted because Base Job already found a solution, ...)

	//An UNSAT result can occur suddenly from any solver. We make sure that only the very first reports the results to Mallob
	// int _NO_UNSAT_REPORT_YET = -1;
	// std::atomic_int  _first_UNSAT_reporting_localId = _NO_UNSAT_REPORT_YET;

	std::vector<int> _worksweeps{}; //to collect statistics
	std::vector<int> _resweeps_in{};
	std::vector<int> _resweeps_out{};
	// shweep_statistics _congruence_stats{};

	Logger _reslogger; //Logging most important results in dedicated file, to not have them mangled by other verbose logs
	Logger _warnlogger; //Logging some warnings in a dedicated file, to avoid needing to grep later the whole large main log files for these warnings

	//the root node tracks the number of sweep iterations and sharing rounds, distributes this information in the sharing operation
	int _root_shared_units_this_iteration = 0;
	int _root_shared_eqs_this_iteration = 0;
	int _root_total_shared_eqs = 0;
	int _root_total_shared_units = 0;
	int _root_emptyrounds_before_progress=0;
	int _root_rounds_this_iteration = 0;
	const int MAX_TOLERATED_EMPTYROUNDS = _params.sweepMaxEmptyRounds.val;
	int _root_sweep_iteration = 0;
	int _root_sharing_round = 0;
	bool _root_did_just_finish_iteration = true; //remember for the next sharing round that we entered a new sweep iteration


	//The root node (and only the root node) tracks progress over the sharing rounds and sweeping iterations
	//It decides whether sharing should continue or whether it should end (either because the last iteration is reached, or because no progress has been made)
	//It broadcasts this decision to all othe ranks, along with general information about the current iteration and round
	//On a technical level, This information is injected here via an inplace root transform at the end of the sharing aggregation, before broadcasting it
	std::function<void(std::vector<int>&)> _inplace_rootTransform = [&](std::vector<int>& payload) {
		assert(_is_root);

		if (_root_did_just_finish_iteration) {
			_root_sweep_iteration++;
			_root_did_just_finish_iteration = false;
			LOG(V1_WARN, "[%i] SWEEP ITERATION %i/%i STARTED \n", _my_rank, _root_sweep_iteration, _params.sweepIterations());
		}

		_root_sharing_round++;
		_root_rounds_this_iteration++;

		int n_units = payload[payload.size() - METADATA_UNIT_SIZE];
		int n_eqs   = payload[payload.size() - METADATA_EQ_SIZE] / 2;

		_root_shared_units_this_iteration += n_units;
		_root_shared_eqs_this_iteration   += n_eqs;

		//Check whether this is yet another round with continued uninterrupted zero progress. Makes only sense to check this once the solvers actually got their work provided.
		if (_root_shared_units_this_iteration==0 && _root_shared_eqs_this_iteration==0) {
			if (_root_provided_initial_work) {
				_root_emptyrounds_before_progress++;
				LOG(V4_VVER, "EMPTYROUND no. %i (iteration %i, sharing round %i)  \n", _root_emptyrounds_before_progress, _root_sweep_iteration, _root_sharing_round);
			} else {
				LOG(V4_VVER, "EMPTYROUND fake, bc. solvers didnt receive work yet (iteration %i, sharing round %i)  \n", _root_sweep_iteration, _root_sharing_round);
			}
		}


		_root_total_shared_units += n_units;
		_root_total_shared_eqs   += n_eqs;

		// LOG(V1_WARN, "[%i] sharing round %i: %i cumul eqs, %i cumul units \n", _my_rank, _root_sharing_round, _total_shared_eqs, _total_shared_units);

		bool terminate_due_to_emptyrounds = false;
		bool all_idle = payload[payload.size() - METADATA_IDLE];
		bool terminate = false;


		if (_root_emptyrounds_before_progress > MAX_TOLERATED_EMPTYROUNDS) {
			terminate_due_to_emptyrounds = true;
			LOG(V1_WARN, "[%i] SWEEP EARLYSTOP in iteration %i, round %i: now %i empty rounds in a row \n", _my_rank, _root_sweep_iteration, _root_sharing_round, _root_emptyrounds_before_progress);
		}

		//A round is finished if all sweepers are idle, i.e. all finished their work.
		if (all_idle || terminate_due_to_emptyrounds) {
			LOG(V1_WARN, "[%i] SWEEP ITERATION %i/%i FINISHED (seen at root transform) with sharing round %i \n", _my_rank, _root_sweep_iteration, _params.sweepIterations(), _root_sharing_round);
			LOG(V1_WARN, "[%i] SWEEP ITERATION %i/%i had: %i EQS, %i UNITS  \n", _my_rank, _root_sweep_iteration, _params.sweepIterations(), _root_shared_eqs_this_iteration, _root_shared_units_this_iteration);
			printSweepStats(_sweepers[_representative_localId], false); //report some intermediate statistics about this iteration
			bool progress = (_root_shared_eqs_this_iteration + _root_shared_units_this_iteration) > 0;
			if (!progress) {
				_root_emptyrounds_before_progress=0; //there never has been progress, so there was never any last round before we found progress
			}
			LOGGER(_reslogger, V2_INFO, "SWEEP_ROUNDS_THIS_ITERATION     %i   \n", _root_rounds_this_iteration);
			LOGGER(_reslogger, V2_INFO, "SWEEP_EMPTYROUNDS_BEFORE_PROGRESS %i   \n", _root_emptyrounds_before_progress);
			LOGGER(_reslogger, V2_INFO, "SWEEP_PROGRESS %i   \n", progress);
			bool lastsweepround = (_root_sweep_iteration == _params.sweepIterations());
			if (lastsweepround || !progress) {
				if (lastsweepround)LOG(V1_WARN, "SWEEP [%i]: Job finished! All iterations done (%i/%i). Broadcasting termination signal with sharing data.\n", _my_rank, _root_sweep_iteration, _params.sweepIterations());
				if (!progress)LOG(V1_WARN, "SWEEP [%i]: Job finished! No more progress in iteration %i/%i. Broadcasting termination signal with sharing data.\n", _my_rank, _root_sweep_iteration, _params.sweepIterations());
				//we DON'T yet set _terminate_all=1 here, because we want also the root solver to first import this last sharing information, which contains valuable equalities and units, before terminating the solvers
				terminate = true;
			}
			else {
				// _root_sweep_round++;
				_root_did_just_finish_iteration = true;
				_root_shared_units_this_iteration = 0;
				_root_shared_eqs_this_iteration = 0;
				_root_emptyrounds_before_progress = 0;
				_root_rounds_this_iteration=0;
				//The new iteration is started by providing  all variables as new work to one solver
				_root_provided_initial_work = false;
				//Prevent that workers see a round change of 2 when going from one sweepround to the next
				// _root_sharing_round--;
			}
		}
		//The root node (and only the root node) tracks the number of completed sweep rounds, and broadcasts this information. This way, also nodes that join later know which round we are in.
		payload[payload.size() - METADATA_SWEEP_ITERATION] = _root_sweep_iteration;
		payload[payload.size() - METADATA_SHARING_ROUND] = _root_sharing_round;
		payload[payload.size() - METADATA_TERMINATE] = terminate;

		assert(!terminate_due_to_emptyrounds || terminate || log_return_false("ERROR unexpected: Sweep root didnt send out terminate signal eventhough it should due to too many emptyrounds "));
		LOG(V3_VERB, "SWEEP root info: Broadcasting SweepIteration %i, Sharing round %i: Eqs %i, Units %i, flags: all_idle(%i), terminate(%i)   \n", _root_sweep_iteration, _root_sharing_round, n_eqs, n_units, all_idle, terminate);
		//no return, payload was just transformed in-place
    };

	enum CustomQuery {
		QUERY_SWEEP_ITERATION = 1
	};


public:
    SweepJob(const Parameters& params, const JobSetup& setup, AppMessageTable& table);
    void appl_start() override;
    void appl_communicate() override;
    void appl_communicate(int sourceRank, int mpiTag, JobMessage& msg) override;
    void appl_terminate() override;
    bool appl_isDestructible() override;

    int appl_solved() override            {return _solved_status;}
    JobResult&& appl_getResult() override {return std::move(_internal_result);}

    void appl_suspend() override {}
    void appl_resume() override {}
    void appl_dumpStats() override {}
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

	bool okToTrackSharingDelay();
	void checkSharingDelay();
	void checkForUnsatResults();
	void tryReportUnsat();
	void reportSolverResult(KissatPtr sweeper, int res);
	void printSweepStats(KissatPtr sweeper, bool full);
	void printCongruenceStats(KissatPtr sweeper);
	// void readResult(KissatPtr shweeper, bool withStats);
	// void serializeResultFormula(KissatPtr sweeper);

	void triggerTerminations();


	bool skip_MPI_forNow();

	void TryWorkstealLocal();
	void TryWorkstealMPI();
	void printIdleFraction();
	void printResweeps();

    void rootInitiateNewSharingRound();
    void cbContributeToAllReduce();
    static std::vector<int> aggregateEqUnitContributions(std::list<std::vector<int>> &contribs);
	static void appendMetadataToReductionElement(std::vector<int> &contrib, int is_idle, int unit_size, int eq_size);
	void advanceAllReduction();

	std::vector<int> getRandomIdPermutation();

	void provideInitialWork(KissatPtr sweeper);
	std::vector<int> stealWorkFromAnyLocalSolver(int asking_rank, int asking_sourceLocalId); //parameters only for verbose logging
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
