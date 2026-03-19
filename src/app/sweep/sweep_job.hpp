
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
	std::atomic_bool _terminated_while_synchronizing{false};
	bool _started_sharedelay_tracking{false};
	int _lastLongtermIdleCount{0};

	//Timing
	float _start_sweep_timestamp;
	std::vector<float> _timestamp_root_started_bcast;
	std::vector<float> _timestamp_receive_sharing_result;
	std::vector<float> _timestamp_contributed_to_sharing;
	std::vector<float> _duration_appl_communicate;

	//Workstealing
	SplitMix64Rng _rng;
	// std::atomic_bool _root_initwork_providable=false;
    std::atomic_bool _root_initwork_startedproviding=false;
    std::atomic_bool _root_initwork_provided=false;
	struct WorkstealRequest {
		int senderLocalId{-1};
		int targetIndex{-1};
		int targetRank{-1};
		std::atomic_bool to_send{false};
		std::atomic_bool got_steal_response{false};
		std::atomic_bool is_inactive{false};
		std::vector<int> stolen_work{};

		void newQueuedRequest(int _senderLocalId) noexcept {
			senderLocalId = _senderLocalId;
			targetIndex = -1;
			targetRank = -1;
			stolen_work.clear();
			//atomic flags only after modifying the non-atomics
			is_inactive = false;
			got_steal_response = false;
			to_send = true;
		}
	};
	std::deque<WorkstealRequest> _worksteal_requests; //deque, because each object has an atomic member and thus isnt copyable (which vector would require)
	const int MIN_STEAL_AMOUNT = 10; //avoid to much overhead at the very end when there is almost no work left, avoid sending around ridiculously small work packages


	//Sharing Equivalences and Units
	// float _root_last_sharing_start_timestamp;
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

	const int NUM_SEARCHING_WORK_FIELDS = 3; //how many fields are attached to an MPI message searching work

	//each aggregation element has some metadata integers at the end
	static const int NUM_METADATA_FIELDS = 9;
		//field indices must be unique numbers exactly filling 1..NUM_METADATA_FIELDS !
		static const int METADATA_WORK_SWEEPS		 = 9;
		static const int METADATA_WORK_STEPOVERS	= 8;
		static const int METADATA_UNSCHED_RESWEEPS = 7;
		static const int METADATA_TERMINATE		  = 6;
		static const int METADATA_SWEEP_ITERATION= 5;
		static const int METADATA_SHARING_ROUND = 4;
		static const int METADATA_IDLE		   = 3;
		static const int METADATA_UNIT_SIZE	  = 2;
		static const int METADATA_EQ_SIZE    = 1;


	//Distribute Eqs and Units that we received from sharing broadcast to local solvers
	static const unsigned INVALID_LIT = UINT_MAX; //Internal literals count unsigned 0,1,2,..., the largest number marks an invalid literal. see further: https://github.com/arminbiere/satch/blob/master/satch.c#L1017
	static const int MAX_IMPORT_SIZE = 400'000; //Limiting the import to a known preallocated area, to simplify concurrent reads and writes (still neglectable with ~ 1.6 MB)
	std::atomic_int _available_import_round{0}; //identifier for the newest import round that we received from the sharing operation
	std::atomic_int _EQS_import_size{0};
	std::atomic_int _UNITS_import_size{0};
	std::vector<int> _EQS_to_import {};
	std::vector<int> _UNITS_to_import {};

	//New Version of Importing, via separated vectors per round
	struct importedRound {
		std::vector<int> eqs{};
		std::vector<int> units{};
	};
	struct finishedCounter {
		std::atomic_int threads_finished_eqs=0;
		std::atomic_int threads_finished_units=0;
	};

	static constexpr int MAX_IMPORT_ROUNDS = 50 * 1000; //Max 50 per seconds (~20ms period), max 1000 seconds
	std::vector<importedRound> _imported_EQS_UNITS{MAX_IMPORT_ROUNDS};
	std::vector<finishedCounter> _finishedRoundCounters{MAX_IMPORT_ROUNDS}; //technically atomics in std::vector, but we only construct once with a fixed size and never push_back or resize, so it compiles and should be fine
	std::atomic_int _lastImportedRound = 0;
	int _lastClearedRound = 0;


	//Termination. Determined during workstealing, broadcasted via sharing
	std::atomic_bool _terminate_all=false; //termination (on this node) due to sharing consensus that there is no more work
	// std::atomic_bool _external_termination=false; //termination because somebody else told us to (for example Job interrupted because Base Job already found a solution, ...)

	//An UNSAT result can occur suddenly from any solver. We make sure that only the very first reports the results to Mallob
	// int _NO_UNSAT_REPORT_YET = -1;
	// std::atomic_int  _first_UNSAT_reporting_localId = _NO_UNSAT_REPORT_YET;

	// std::vector<int> _worksweeps{}; //to collect statistics
	// std::vector<int> _resweeps_in{};
	// std::vector<int> _resweeps_out{};

	Logger _reslogger;  //Logging most important results in dedicated file, to not have them mangled by other verbose logs
	Logger _warnlogger; //Logging some warnings in a dedicated file, to avoid needing to grep later the whole large main log files for these warnings
	// Logger _rootlogger; //Logging information from the root transformation

	//the root node tracks the number of sweep iterations and sharing rounds, distributes this information in the sharing operation
	int _root_shared_units_this_iteration = 0;
	int _root_shared_eqs_this_iteration = 0;
	int _root_total_shared_eqs = 0;
	int _root_total_shared_units = 0;
	int _root_emptyrounds_before_progress=0;
	int _root_rounds_this_iteration = 0;
	int _root_sweep_iteration = 0;
	int _root_sharing_round = 0;
	bool _root_did_just_finish_iteration = true; //remember for the next sharing round that we entered a new sweep iteration. Start with true to immediately start into iteration 1

	const int MAX_TOLERATED_EMPTYROUNDS = _params.sweepMaxEmptyRounds.val;

	//The root node (and only the root node) tracks progress over the sharing rounds and sweeping iterations
	//It decides whether sharing should continue or whether it should end (either because the last iteration is reached, or because no progress has been made)
	//It broadcasts this decision to all othe ranks, along with general information about the current iteration and round
	//On a technical level, This information is injected here via an inplace root transform at the end of the sharing aggregation, before broadcasting it
	std::function<void(std::vector<int>&)> _inplace_rootTransform = [&](std::vector<int>& payload) {
		assert(_is_root);
		_root_sharing_round++;
		LOG(V2_INFO, "SWEEP [%i](root-trf) rnd(%i) entered \n", _my_rank, _root_sharing_round);

		//Remember from last sharing round whether now begins a new iteration iteration
		if (_root_did_just_finish_iteration) {
			_root_sweep_iteration++;
			_root_did_just_finish_iteration = false;

			//Only now the new iteration truly begins, so only now we reset these iteration-specific counters
			_root_shared_units_this_iteration = 0;
			_root_shared_eqs_this_iteration = 0;
			_root_emptyrounds_before_progress = 0;
			_root_rounds_this_iteration=0;

			LOG(V2_INFO, "SWEEP [%i](root-trf) ITERATION %i/%i STARTED \n", _my_rank, _root_sweep_iteration, _params.sweepMaxIterations());
		}

		int n_units	 = payload[payload.size() - METADATA_UNIT_SIZE];
		int n_eqs	 = payload[payload.size() - METADATA_EQ_SIZE] / 2;  //each equivalence takes up two integers
		bool all_idle= payload[payload.size() - METADATA_IDLE];
		int work_sweeps			 = payload[payload.size() - METADATA_WORK_SWEEPS];
		int work_stepovers		 = payload[payload.size() - METADATA_WORK_STEPOVERS];
		int work_unsched_resweeps= payload[payload.size() - METADATA_UNSCHED_RESWEEPS];

		_root_shared_units_this_iteration += n_units;
		_root_shared_eqs_this_iteration   += n_eqs;
		_root_total_shared_units += n_units;
		_root_total_shared_eqs   += n_eqs;

		_root_rounds_this_iteration++;


		//Check whether this is a round with yet again zero progress. It makes only sense to check for progress once the solvers actually got their work provided.
		if (_root_shared_units_this_iteration==0 && _root_shared_eqs_this_iteration==0) {
			//we don't count empty sharing rounds if we know that there hasn't even been work provided, i.e. solvers couldn't even do anything till now
			if (_root_initwork_provided) {
				_root_emptyrounds_before_progress++;
				LOG(V2_INFO, "SWEEP [%i](root-trf) EMPTYROUND nr %i (iteration %i, sharing round %i)  \n", _my_rank, _root_emptyrounds_before_progress, _root_sweep_iteration, _root_sharing_round);
			} else {
				LOG(V2_INFO, "SWEEP [%i](root-trf) fake EMPTYROUND, because solvers didnt receive work yet (iteration %i, sharing round %i)  \n", _my_rank, _root_sweep_iteration, _root_sharing_round);
			}
		}

		bool send_terminate = false;
		bool terminate_emptyrounds = _root_emptyrounds_before_progress > MAX_TOLERATED_EMPTYROUNDS;

		if (terminate_emptyrounds) {
			LOG(V2_INFO, "SWEEP [%i](root-trf) EARLYSTOP in iteration %i, round %i: now %i empty rounds in a row \n", _my_rank, _root_sweep_iteration, _root_sharing_round, _root_emptyrounds_before_progress);
		}

		//A round is finished if all sweepers are idle, or if we had for too long exclusively empty rounds since the start of this iteration
		if (all_idle || terminate_emptyrounds) {
			LOG(V2_INFO, "SWEEP [%i](root-trf) (%i)all_idle  (%i)terminate_emptyrounds \n", _my_rank, all_idle, terminate_emptyrounds);
			LOG(V2_INFO, "SWEEP [%i](root-trf) ITERATION %i/%i FINISHED (seen at root transform) in sharing round %i \n", _my_rank, _root_sweep_iteration, _params.sweepMaxIterations(), _root_sharing_round);
			LOG(V2_INFO, "SWEEP [%i](root-trf) ITERATION %i/%i shared: %i EQS, %i UNITS  \n", _my_rank, _root_sweep_iteration, _params.sweepMaxIterations(), _root_shared_eqs_this_iteration, _root_shared_units_this_iteration);
			//The kissat solver will report the sweep stats itself via a callback once it has cleaned up its internal database and metrics via substitute()
			// printSweepStats(_sweepers[_representative_localId], false); //report some intermediate statistics about this iteration
			bool progress = (_root_shared_eqs_this_iteration + _root_shared_units_this_iteration) > 0;
			if (!progress) {
				_root_emptyrounds_before_progress=0; //there never has been progress, so there was never any last round before we found progress
			}
			bool lastsweepround = (_root_sweep_iteration == _params.sweepMaxIterations());
			if (lastsweepround || !progress) {
				if (lastsweepround) LOG(V2_INFO, "SWEEP [%i](root-trf): Job finished! All iterations done (%i/%i). Broadcasting termination signal with sharing data.\n", _my_rank, _root_sweep_iteration, _params.sweepMaxIterations());
				if (!progress)		LOG(V2_INFO, "SWEEP [%i](root-trf): Job finished! No more progress in iteration %i/%i. Broadcasting termination signal with sharing data.\n", _my_rank, _root_sweep_iteration, _params.sweepMaxIterations());
				//we DON'T yet set _terminate_all=1 here, because we want also the root solver to first import this last sharing information, which contains valuable equalities and units, before terminating the solvers
				send_terminate = true;
			}
			else {
				_root_did_just_finish_iteration = true;
				//The new iteration is started by providing  all variables as new work to one solver
				// _root_initwork_providable	= false;
				_root_initwork_startedproviding = false;
				_root_initwork_provided		= false;
				//Prevent that workers see a round change of 2 when going from one sweepround to the next
				// _root_sharing_round--;
				LOG(V2_INFO, "SWEEP [%i](root-trf) Preparing for new iteration. Have set (%i)started_providing_work, (%i)provided_work  \n", _my_rank, _root_initwork_startedproviding.load(), _root_initwork_provided.load() );
			}
		}
		//The root node (and only the root node) tracks the number of completed sweep rounds, and broadcasts this information. This way, also nodes that join later know which round we are in.
		payload[payload.size() - METADATA_SWEEP_ITERATION] = _root_sweep_iteration;
		payload[payload.size() - METADATA_SHARING_ROUND] = _root_sharing_round;
		payload[payload.size() - METADATA_TERMINATE] = send_terminate;
		//the all_idle payload is already set

		assert(!terminate_emptyrounds || send_terminate || log_return_false("SWEEP ERROR unexpected: Sweep root didnt send out terminate signal eventhough it should due to too many emptyrounds "));

		char logmsg[512];
		snprintf(logmsg, sizeof(logmsg),
			"SWEEP [%i](root-trf) send: iter %i rnd %i :  %i ai  %i trm  E %i  U %i    SW %i  ST %i  RE %i      WW  %i \n",
			_my_rank, _root_sweep_iteration, _root_sharing_round, all_idle, send_terminate, n_eqs, n_units,
			work_sweeps, work_stepovers, work_unsched_resweeps, work_sweeps + work_stepovers
		);

		//Log two times, once for completeness chronologically in the general logs, and once in a special root file for easier postprocessing later
		LOG(			   V2_INFO, "%s", logmsg);
		LOGGER(_reslogger, V2_INFO, "%s", logmsg);

		// LOG(V2_INFO,				 "SWEEP [%i](root-trf) send: Iter(%i) rnd(%i): (%i)ai (%i)trm  E %i  U %i    SW %i  ST %i  RE %i  WW (%i)   \n", _my_rank, _root_sweep_iteration, _root_sharing_round, all_idle, send_terminate, n_eqs, n_units, work_sweeps, work_stepovers,  work_unsched_resweeps, work_sweeps + work_stepovers);
		// LOGGER(_rootlogger, V2_INFO, "SWEEP [%i](root-trf) send: Iter(%i) rnd(%i): (%i)ai (%i)trm  E %i  U %i    SW %i  ST %i  RE %i  WW (%i)   \n", _my_rank, _root_sweep_iteration, _root_sharing_round, all_idle, send_terminate, n_eqs, n_units, work_sweeps, work_stepovers,  work_unsched_resweeps, work_sweeps + work_stepovers);
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
	friend int  cb_custom_query(void *SweeJobState, int query);
	friend void cb_report_iteration(void *SweepJobState, int localId);


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
	void reportEndStats(KissatPtr sweeper);
	void printCongruenceStats(KissatPtr sweeper);

	void triggerTerminations();


	bool skip_MPI_forNow();


	void solverGoStealing(KissatPtr sweeper);
	void sendWorkstealsViaMPI();
	void printIdleWorkStatus();
	// void printResweeps();

    void rootStartNewSharingRound();
    void cbContributeToAllReduce();
    static std::vector<int> aggregateEqUnitContributions(std::list<std::vector<int>> &contribs);
	static void appendMetadataToReductionElement(std::vector<int> &contrib, int is_idle, int unit_size, int eq_size, int work_sweeps, int work_stepovers, int unsched_resweeps);
	void advanceAllReduction();
	void extractAllReductionResult();

	std::vector<int> getRandomIdPermutation();

	bool tryProvideInitialWork(KissatPtr sweeper);
	std::vector<int> stealWorkFromAnyLocalSolver(int asking_rank, int asking_sourceLocalId); //parameters only for verbose logging
    std::vector<int> stealWorkFromSpecificLocalSolver(int localId);
    // void cbStealWork(unsigned **work, int *work_size, int localId);
	void cbStealWorkNew(unsigned **work, int *work_size, int localId);
	void checkForNewImportRound(KissatPtr sweeper);
	void cbImportEq(int *ilit1, int *ilit2, int localId);
	void cbImportUnit(int *lit, int localId);
	int  cbCustomQuery(int query);
	void cbReportIteration(int localId);
	void clearImportedRound();

	virtual ~SweepJob();

};

#endif
