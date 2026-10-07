#ifndef DOMPASCH_MALLOB_SWEEP_JOB_HPP
#define DOMPASCH_MALLOB_SWEEP_JOB_HPP

#include <shared_mutex>
#include <deque>
#include <cstring>

#include "app/job.hpp"
#include "../sat/solvers/kissat.hpp"
#include "app/sat/job/anytime_sat_clause_communicator.hpp"
#include "comm/job_tree_all_reduction.hpp"
#include "comm/job_tree_broadcast.hpp"
#include "app/sat/solvers/solver_portfolio_config.hpp"

#include "app/sweep/kissat_sweep_extras.hpp"


class SweepJob : public BaseSatJob {
private:

    JobResult _internal_result;
	//final status that gets communicated to Mallob, only touched by the main thread
    int _solved_status{-1};
	//can be touched by any thread
	int _staged_solved_status{-1};
	bool _do_report_UNSAT_to_root{false};
	std::atomic<int> _root_reported_result{-1};
	bool _finished_job_setup{false};
	bool _started_communication{false};
	bool _exited_immediately{false};

	// bool _started_appl_start{false};
    int _my_rank{0};
    int _my_index{0};
	int _my_ctx_id{0};
    bool _is_root{false};
    uint8_t* _metadata; //serialized description
	int _numOrigVars{0};
	// int _numOrigClauses{0};

	const int INVALID_ELIT = __INT32_MAX__;

	//a dedicated solver that reports its statistics to us
	const int _representative_localId{0};

	//Local Solvers
	int _nThreads{0};
	typedef std::shared_ptr<KissatSweep> KissatPtr;
	std::vector<KissatPtr> _sweepers;
	std::vector<std::unique_ptr<BackgroundWorker>> _bg_workers;
    std::atomic_int _started_sweepers_count {0};
    std::atomic_int _running_sweepers_count {0};
	std::atomic_int _finished_sweepers_count {0};
	std::vector<int> _list_of_ids;
	std::atomic_bool _flag_started_synchronized_solving{false};
	std::atomic<float> _timestamp_started_synchronized_solving{0};
	std::atomic_bool _flag_terminated_while_synchronizing{false};
	std::atomic_bool _root_finished_CCC{false};
	int _lastLongtermIdleCount{0};

	//Timing
	float			   _timestamp_start_sweepapp = 0;
	std::vector<float> _timestamp_root_started_bcast;
	std::vector<float> _timestamp_receive_sharing_result;
	std::vector<float> _timestamp_contributed_to_sharing;
	// std::vector<float> _duration_appl_communicate;
	float				_max_appl_comm_duration=0;
	bool 			   _logged_full_jobcomm{false};
	float			   _timestamp_log_last_idleinfo = 0;
	float			   _timestamp_log_delayedround = 0;
	float			   _timestamp_log_notonline = 0;
	float			   _timestamp_log_sharingdelay = 0;

	//Workstealing
	SplitMix64Rng _rng;
	// std::atomic_bool _root_initwork_providable=false;
    std::atomic_bool _root_initwork_startedproviding=false;
    std::atomic_bool _root_initwork_provided=false;
	std::atomic_bool _rank_is_inbetween_iterations=true;
	struct WorkstealRequest {
		int senderLocalId{-1};
		int targetIndex{-1};
		int targetRank{-1};
		float t_queued{0};
		float t_received{0};
		float t_read{0};
		int nr{-1};
		std::atomic_bool to_send{false};
		std::atomic_bool got_steal_response{false};
		std::atomic_bool is_active{false};
		std::vector<int> stolen_work{};

		void newQueuedRequest(int _senderLocalId, int _nr) noexcept {
			senderLocalId = _senderLocalId;
			targetIndex = -1;
			targetRank = -1;
			stolen_work.clear();
			t_queued = Timer::elapsedSeconds();
			t_received = -1;
			t_read = -1;
			nr = _nr;
			//atomic flags are changed only now, after modifying the non-atomics
			is_active = true;
			got_steal_response = false;
			to_send = true;
		}
	};
	//deque, because each object has an atomic member and thus isnt copyable (which vector would require)
	std::deque<WorkstealRequest> _worksteal_requests;
	//prevent excessivley small steals at the end
	const int MIN_STEAL_AMOUNT = 2;

	//Sharing Equivalences and Units
    std::unique_ptr<JobTreeBroadcast> _bcast;
    std::unique_ptr<JobTreeAllReduction> _red;

	//Sanity checks, Warn if periods between sharing rounds get too large
	float _last_received_sharing_time{0};
	float _last_contribution_time{0};
	float _last_sharedelay_warning{0};

    const int TAG_SEARCHING_WORK= 1001;
    const int TAG_RETURNING_STEAL_REQUEST = 1002;
    const int TAG_BCAST_INIT	= 1003;
    const int TAG_ALLRED		= 1004;
	const int TAG_FOUND_UNSAT	= 1005;

	//how many fields are attached to an MPI message searching work
	const int NUM_SEARCHING_WORK_FIELDS = 3;

	//Bundled metadata payload that lives at the tail of every reduction element.
	//Stored via memcpy onto a std::vector<int>, so the layout must be plain ints with no padding.
	//Adding a new field here is the only change required: NUM_METADATA_FIELDS is derived from sizeof.
	struct Metadata {
		//Per-solver fields, written before contributing
		int foundUnsat{0};
		int idle_count{0};
		int longtermidle_count{0};
		int active_count{0};
		int working_internally_count{0};
		int lagging{0};
		int sweeper_objs{0};
		int remaining_work_estimate{0};
		int unit_size{0};
		int eq_size{0};
		int work_sweeps{0};
		int work_stepovers{0};
		int unsched_resweeps{0};
		int maxxed_kittens{0};
		//Root-transform fields, written at the root after the reduction finishes
		int sweep_iteration{0};
		int sharing_round{0};
		int end_iteration{0};
		int terminate{0};
		int env_completions{0};
	};
	static_assert(sizeof(Metadata) % sizeof(int) == 0,
		"Metadata must be a packed run of ints for tail-of-vector memcpy");
	static constexpr int NUM_METADATA_FIELDS = sizeof(Metadata) / sizeof(int);
	
	static constexpr int SWEEPRESULT_METADATA_FIELDS = 2;

	//Buffer received Eq+Units from sharing rounds, for Sweepers to soon import them
	//To allow easier concurrent accessed, we choose a large preallocated vector
	//Should be enough for 5000 second runs with very aggressive 20ms sharing rounds (50 per second)
	//This is still cheap memory-wise, since each entry only stores the references to the actual Eq+Unit vectors
	struct importedRound {
		std::vector<int> eqs{};
		std::vector<int> units{};
	};
	static constexpr int MAX_IMPORT_ROUNDS = 5000 * 50;
	std::vector<importedRound> _imported_data{MAX_IMPORT_ROUNDS};


	//After all sweepers have imported a specific round, we no longer need to buffer it
	//Here we technically we store atomics in std::vector,
	//but we only construct once with a fixed size and never push_back or resize, so it compiles and should be fine
	//After all solvers have picked up the shared data, it can be deleted from this rank
	int _lastClearedRound = 0;
	struct finishedCounter {
		std::atomic_int threads_finished_eqs=0;
		std::atomic_int threads_finished_units=0;
	};
	std::vector<finishedCounter> _finishedRoundCounters{MAX_IMPORT_ROUNDS};
	std::atomic_int _lastImportedRound = 0;

	//Map a specific round to an iteration. The value -9 is just a sentinel/dummy
	std::vector<int> _iteration_of_round = std::vector<int>(MAX_IMPORT_ROUNDS, -9);

	//For a very niche situation we need to know the expected _next_ iteration number
	int expected_iteration_of_next_round = -1;

	//Keep track of Eq+Unit success as well as the number of swept variables,
	//their ratio determins whether we skip iterations and potentially terminate the entire job
	std::vector<int> _shared_EU_this_iteration_cumul{};
	std::vector<int> _swept_this_iteration_cumul{};


	//The user defines the skip window in terms of seconds
	//For us internally, it is however more elegant to express it in the corresponding number of sharing rounds
	//that happen within that time window
	//since this naturally ignores time spent inbetween iterations (where sequential bookkeping is done)
	//and where skipping would not defined or wanted
	int _skip_window_rounds = _params.sweepSkipWindowSecs() / (double) _params.sweepSharingPeriod();

	//See how much each rank contributed in postprocessing
	//Main use is to detect whether some ranks didn't contribute at all, which would hint at a bug
	int _rank_contributed_equalities = 0;
	int _rank_contributed_units = 0;

	//Terminate the sweep job/app. Either self-determined, or received by an external termination
	std::atomic_bool _terminate_all=false;

	Logger _sweeplogger;

	//Solver configuration is done via a JSON file, here the 'sweeper' flavour applies
	SolverPortfolioConfig _solverConfig;


	//when we exporting Eqs+Units from a solver thread to Mallob, use mutex to prevent
	//the solver thread to concurrently push new data onto the vector we are just reading/moving
	std::mutex _stealinfo_mutex;
	std::vector<std::vector<SweepStealInfo>> _stealinfos_per_solver;

	//the root node tracks the number of sweep iterations and sharing rounds,
	//distributes this information in the sharing operation
	int _root_iteration = 0;
	int _root_sharing_round = 0;
	int _root_rounds_this_iteration = 0;
	int _root_shared_units_this_iteration = 0;
	int _root_shared_eqs_this_iteration = 0;
	int _root_total_shared_eqs = 0;
	int _root_total_shared_units = 0;
	
	int _root_atp_EU = 0; //all time peak Eq+Units in one sharing round (carries over to next iterations)
	int _root_atp_round = 0; //round of that atp value (reset to 0 at new iteration)

	//This next value starts with true to immediately start into iteration nr. 1
	bool _root_did_just_finish_iteration = true;

	int _root_skipped_iterations = 0;
	int _root_weak_iterations = 0;
	bool _root_had_success_this_iteration = false;
	bool _root_had_work_this_iteration = false;

	//Cross-Job-sharing
	std::unique_ptr<AnytimeSatClauseCommunicator> _clause_comm;
	std::vector<int>  _crossjob_root_received_units{};
	std::mutex _crossjob_import_mutex;
	int _clausecomm_isDestructible_counter=0;

	//[seconds] End sweeping earlier than the wallclock time, to allow for substitute to finish,
	//to get a proper final clause database state before reporting
	const double TIMEBUFFER_FOR_FINAL_SUBSTITUTE = 1;

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

    int appl_solved() override {
		return _solved_status;
	}
    JobResult&& appl_getResult() override {return std::move(_internal_result);}

    void appl_suspend() override {}
    void appl_resume() override {}
    void appl_dumpStats() override {}

    void appl_memoryPanic() override;

    friend void cb_search_work_in_tree(void* SweepJob_state, unsigned **work, int *work_size, int local_id);
	friend void cb_import_eq(void *SweepJobState, int *elit1, int *elit2, int localId);
	friend void cb_import_unit(void *SweepJobState, int *elit, int localId);
	friend int  cb_custom_query(void *SweeJobState, int localId, int query);
	friend void cb_report_iteration(void *SweepJobState, int localId);
	
	struct SweepResult {
		std::vector<int> units{};
		std::vector<int> eqs{}; 
		std::vector<Kissat::namedSolverArray> reconstruction;
	};

	static inline const std::string SWEEPRESULT_DIR_KEY = "sweepresult-dir";
	//(De)serialization  SweepResult <---> int-vector
	
	// static std::vector<int> serializeSweepResult(const SweepResult &resObj);	
	// static SweepResult deserializeSweepResult(const std::vector<int> &resVec);
	
	//Conversion jsonstring  <---> Extra data in SweepResult
	// static std::string getSweepResultFromJson(const SweepResult &res);
	// static void addJsonToSweepResult(SweepResult &res, const std::string &jsonstring);
	
	static SweepResult readSweepResultsFromDir(const std::string &dir);
	static void printSweepResult(const SweepResult &res);

private:
	KissatPtr createNewSweeper(int localId);

	void createAndStartNewSweeper(int localId);
    void loadFormula(KissatPtr sweeper);
	
	SweepResult collectSweepResult(std::vector<Kissat::namedSolverArray> &reconstruction);
	std::string writeSweepResultsToDir(const SweepResult &res);
	
	void _inplace_rootTransform(std::vector<int>& payload);
	
	static size_t bytesToInts(size_t bytes);

	void checkSharingDelay();
	void checkForUnsatResults();
	void rootReportSolverResult(int res, const KissatPtr &sweeper);
	void reportEndStats(KissatPtr sweeper);
	void tryReportToMallob();
	bool checkCrossCommNeedsAdvancing(const std::string &from);
	void reportStealLatencies(KissatPtr sweeper);
	void triggerTerminations();

	bool skip_MPI_forNow();

	void solverGoStealing(KissatPtr sweeper);
	void sendWorkstealsViaMPI();
	void checkIdleWorkStatus();
	int countLaggingSolvers();
	bool isSolverLagging(KissatPtr sweeper);

    void rootStartNewSharingRound();
    void cbContributeToAllReduce();
    static std::vector<int> aggregateEqUnitContributions(std::list<std::vector<int>> &contribs);
	static void appendMetadataToReductionElement(std::vector<int> &contrib, const Metadata &md);
	static Metadata readMetadataFromReductionElement(const std::vector<int> &contrib);
	static void writeMetadataToReductionElement(std::vector<int> &contrib, const Metadata &md);
	void advanceAllReduction();
	void extractAllReductionResult();

	void crossjob_rootReceiveClauses(std::vector<int>  &&clauses);

	std::vector<int> getRandomIdPermutation();
	void printActiveMPIRequestsCount();
	void printFirstClauses(const std::vector<int> &formula, int nbClauses);

	bool canSolverExitStealing(KissatPtr sweeper);
	bool tryProvideInitialWork(KissatPtr sweeper);
	std::vector<int> stealWorkFromAnyLocalSolver(int asking_rank, int asking_sourceLocalId); //parameters only for verbose logging
    std::vector<int> stealWorkFromSpecificLocalSolver(int localId);
	void cbStealWorkNew(unsigned **work, int *work_size, int localId);
	void cbImportEq(int *elit1, int *elit2, int localId);
	void cbImportUnit(int *lit, int localId);
	int  cbCustomQuery(int localId, int query);
	void cbReportIteration(int localId);
	void clearImportedRound();

	virtual ~SweepJob();

	//stubs
	bool isInitialized() override {
		LOGGER(_sweeplogger,V4_VVER, "[SweepJob] Called stub: isInitialized\n");
		return true;
	}

	void prepareSharing() override {
		LOGGER(_sweeplogger,V4_VVER, "[SweepJob] Called stub: prepareSharing\n");
		//we prepare sharing anyway in every sharing round, don't need this reminder/callback(?)
	}

	bool hasPreparedSharing() override {
		//Always returning true to be on the safe side, i.e. never blocking the advancement of allreduce
		//Especially here in sweep, where all the CrossJob sharing is funneled through the root rank 
		//i.e. all lower ranks never contribute directly to the CrossSharing object
		return true;
	}


	std::vector<int> getPreparedClauses(Checksum& checksum, int& successfulSolverId, int& numLits) override {
		successfulSolverId = -1;
		numLits = 0;
		LOGGER(_sweeplogger,V4_VVER, "[SweepJob] Called stub: getPreparedClauses. return succSolver -1 , numLits 0, vector {}\n");
		return {};
	}

	void filterSharing(int, std::vector<int>&&) override {
		LOGGER(_sweeplogger,V4_VVER, "[SweepJob] Called stub: filterSharing\n");
	}

	bool hasFilteredSharing(int) override {
		LOGGER(_sweeplogger,V4_VVER, "[SweepJob] Called stub: hasFilteredSharing. return true\n");
		return true;
	}

	std::vector<int> getLocalFilter(int) override {
		LOGGER(_sweeplogger,V4_VVER, "[SweepJob] Called stub: getLocalFilter. return {}\n");
		return {};
	}

	void applyFilter(int, std::vector<int>&&) override {
		LOGGER(_sweeplogger,V4_VVER, "[SweepJob] Called stub: applyFilter\n");
	}

	void digestSharingWithoutFilter(int epoch, std::vector<int>  &&clauses, bool stateless) override {
		//We only receive at the root node, all further distribution is handled by our own SweepApp logic
		if (_is_root) {
			InplaceClauseAggregation(clauses).stripToRawBuffer(); //found by Claude, counterpart to InplaceClauseAggregation agg(msg.payload);
			LOGGER(_sweeplogger,V3_VERB, "SWEEP receive XTCS (stripped) size %i\n",clauses.size());
			crossjob_rootReceiveClauses(std::move(clauses));
		}
	}

	void returnClauses(std::vector<int>&&) override {
		LOGGER(_sweeplogger,V4_VVER, "[SweepJob] Called stub: returnClauses\n");
	}

	void digestHistoricClauses(int, int, std::vector<int>&&) override {
		LOGGER(_sweeplogger,V4_VVER, "[SweepJob] Called stub: digestHistoricClauses\n");
	}

	int getLastAdmittedNumLits() override {
		LOGGER(_sweeplogger,V4_VVER, "[SweepJob] Called stub: getLastAdmittedNumLits. return 0\n");
		return 0;
	}

	long long getBestFoundObjectiveCost() override {
		LOGGER(_sweeplogger,V4_VVER, "[SweepJob] Called stub: getBestFoundObjectiveCost. return 0\n");
		return 0;
	}

	void setClauseBufferRevision(int) override {
		LOGGER(_sweeplogger,V4_VVER, "[SweepJob] Called stub: setClauseBufferRevision\n");
	}

	void updateBestFoundSolutionCost(long long) override {
		LOGGER(_sweeplogger,V4_VVER, "[SweepJob] Called stub: updateBestFoundSolutionCost\n");
	}

};

#endif
