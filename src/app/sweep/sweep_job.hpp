#ifndef DOMPASCH_MALLOB_SWEEP_JOB_HPP
#define DOMPASCH_MALLOB_SWEEP_JOB_HPP

#include <shared_mutex>
#include <deque>

#include "app/job.hpp"
#include "../sat/solvers/kissat.hpp"
#include "app/sat/job/anytime_sat_clause_communicator.hpp"
#include "comm/job_tree_all_reduction.hpp"
#include "comm/job_tree_broadcast.hpp"


// #define IMPORT_TECHNIQUE 3

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

	bool _started_appl_start{false};
    int _my_rank{0};
    int _my_index{0};
	int _my_ctx_id{0};
    bool _is_root{false};
    uint8_t* _metadata; //serialized description
	int _numVars{0};

	const int INVALID_ELIT = __INT32_MAX__;

	//a dedicated solver that reports its statistics to us
	const int _representative_localId{0};
	const int _congruence_localId{1};

	//Local Solvers
	int _nThreads{0};
	typedef std::shared_ptr<Kissat> KissatPtr;
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
	std::vector<float> _duration_appl_communicate;
	bool 			   _logged_full_jobcomm{false};
	float			   _timestamp_last_idleinfo = 0;

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

	//each aggregation element has some metadata at the end
	//field indices must be unique numbers exactly filling 1..NUM_METADATA_FIELDS !
	//Enums would be more elegant, but keeping this for now
	static const int NUM_METADATA_FIELDS = 15;
		static const int METADATA_MAXXED_KITTENS = 15;
		static const int METADATA_LONGTERM_IDLE	  = 14;
		static const int METADATA_FOUND_UNSAT	   = 13;
		static const int METADATA_ACTIVE_COUNT	    = 12;
		static const int METADATA_ENVCOMPLETIONS     = 11;
		static const int METADATA_END_ITERATION		  = 10;
		static const int METADATA_WORK_SWEEPS		 = 9;
		static const int METADATA_WORK_STEPOVERS	= 8;
		static const int METADATA_UNSCHED_RESWEEPS = 7;
		static const int METADATA_TERMINATE		  = 6;
		static const int METADATA_SWEEP_ITERATION= 5;
		static const int METADATA_SHARING_ROUND = 4;
		static const int METADATA_IDLE_COUNT   = 3;
		static const int METADATA_UNIT_SIZE	  = 2;
		static const int METADATA_EQ_SIZE    = 1;

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

	//See how much each rank contributed in postprocessing
	//Main use is to detect whether some ranks didn't contribute at all, which would hint at a bug
	int _rank_contributed_equalities = 0;
	int _rank_contributed_units = 0;

	//Terminate the sweep job/app. Either self-determined, or received by an external termination
	std::atomic_bool _terminate_all=false;

	Logger _sweeplogger;

	//when we exporting Eqs+Units from a solver thread to Mallob, use mutex to prevent
	//the solver thread to concurrently push new data onto the vector we are just reading/moving
	std::mutex _stealinfo_mutex;
	std::vector<std::vector<SweepStealInfo>> _stealinfos_per_solver;

	//the root node tracks the number of sweep iterations and sharing rounds,
	//distributes this information in the sharing operation
	int _root_shared_units_this_iteration = 0;
	int _root_shared_eqs_this_iteration = 0;
	int _root_total_shared_eqs = 0;
	int _root_total_shared_units = 0;
	int _root_rounds_this_iteration = 0;
	int _root_sweep_iteration = 0;
	int _root_sharing_round = 0;
	//Starts with true to immediately start into iteration 1.
	bool _root_did_just_finish_iteration = true;
	int _root_skipped_iterations = 0;

	//Cross-Job-sharing
	std::unique_ptr<AnytimeSatClauseCommunicator> _clause_comm;
	std::unique_ptr<GenericClauseStore> _clause_store;
	std::vector<int>  _crossjob_root_received_units{};
	std::mutex _crossjob_import_mutex;
	bool _crossjob_has_prepared_sharing{false};

	//[seconds] End sweeping 10 seconds earlier than the wallclock time, to allow for substitute to finish,
	//to get a proper final clause database state before reporting
	const double TIMEBUFFER_FOR_FINAL_SUBSTITUTE = 10;

	//The root node (and only the root node) tracks global sweeping progress
	//It decides whether a given sharing iteration should continue or end
	//It broadcasts this decision to all other ranks, along with general information about the current sweeping state
	//On a technical level, this information is injected here via an inplace root transform at
	//the end of the sharing aggregation, before broadcasting it
	std::function<void(std::vector<int>&)> _inplace_rootTransform = [&](std::vector<int>& payload) {
		assert(_is_root);
		_root_sharing_round++;
		//Remember from last sharing round whether now begins a new iteration
		if (_root_did_just_finish_iteration) {
			_root_sweep_iteration++;
			_root_did_just_finish_iteration = false;
			_shared_EU_this_iteration_cumul = {0};
			_swept_this_iteration_cumul = {0};

			//Only now the new iteration truly begins, so only now we reset these iteration-specific counters
			_root_shared_units_this_iteration = 0;
			_root_shared_eqs_this_iteration = 0;
			_root_rounds_this_iteration=0;

			LOGGER(_sweeplogger,V2_INFO, "SWEEP [%i](root-trf) ITERATION %i/%i STARTED \n", _my_rank, _root_sweep_iteration, _params.sweepMaxIterations());
		}
		int n_sweep_units = payload[payload.size() - METADATA_UNIT_SIZE];
		int eq_size  = payload[payload.size() - METADATA_EQ_SIZE];
		int n_eqs	 = eq_size / 2;  //each equivalence takes up two integers
		int idle_count = payload[payload.size() - METADATA_IDLE_COUNT];
		int longtermidle_count = payload[payload.size() - METADATA_LONGTERM_IDLE];
		int active_count = payload[payload.size() - METADATA_ACTIVE_COUNT];
		int work_sweeps			 = payload[payload.size() - METADATA_WORK_SWEEPS];
		int work_stepovers		 = payload[payload.size() - METADATA_WORK_STEPOVERS];
		int work_unsched_resweeps= payload[payload.size() - METADATA_UNSCHED_RESWEEPS];
		int foundUnsat			 = payload[payload.size() - METADATA_FOUND_UNSAT];
		int maxxed_kittens		= payload[payload.size() - METADATA_MAXXED_KITTENS];

		bool all_idle = (active_count == 0);
		double done_scheduled_prcnt = 100*(work_sweeps + work_stepovers)/(double)_numVars;
		_root_shared_units_this_iteration += n_sweep_units;
		_root_shared_eqs_this_iteration   += n_eqs;
		_root_total_shared_units += n_sweep_units;
		_root_total_shared_eqs   += n_eqs;
		_root_rounds_this_iteration++;
		_shared_EU_this_iteration_cumul.push_back(_root_shared_units_this_iteration + _root_shared_eqs_this_iteration);
		_swept_this_iteration_cumul.push_back(work_sweeps + work_unsched_resweeps);
		bool decide_end_iteration = false;
		bool decide_terminate_job = false;
		if (all_idle) {
			LOGGER(_sweeplogger,V2_INFO, "SWEEP [%i](root-trf): All idle - ending this iteration %i \n", _my_rank, _root_sweep_iteration);
			decide_end_iteration = true;
		}
		auto &shared = _shared_EU_this_iteration_cumul;
		auto &swept = _swept_this_iteration_cumul;
		int window = _params.sweepSkipWindow();
		if (window > shared.size()) {
			window = shared.size();
		}
		//Always calculate the success within the window of the last rounds, to print it
		int shared_in_window = shared.back() - shared[shared.size()-window];
		int swept_in_window  = swept.back()  - swept[swept.size()-window];
		double success_in_window = swept_in_window==0 ? 0: shared_in_window / (double) swept_in_window;
		//Decide whether to skip the iteration only if we have accumulated enough rounds
		if (shared.size()>=_params.sweepSkipWindow()) {
			if (success_in_window < _params.sweepSkipRatio()) {
				decide_end_iteration = true;
				_root_skipped_iterations++;
				LOGGER(_sweeplogger,V2_INFO, "SWEEP [%i](root-trf) SKIP iteration %i (rnd %i), success %f (%i / %i) < %.3f (skip-threshhold) , in rounds [%i, %i]. Is skip nr. %i  \n",
					_my_rank, _root_sweep_iteration, _root_sharing_round,  success_in_window, shared_in_window, swept_in_window, _params.sweepSkipRatio(), _root_sharing_round - window, _root_sharing_round, _root_skipped_iterations);
			}
		}
		if (_root_skipped_iterations > _params.sweepSkipCount()) {
			decide_terminate_job = true;
			LOGGER(_sweeplogger,V2_INFO, "SWEEP [%i](root-trf) TERMINATE job , due to %i th skipped iteration \n", _my_rank, _root_skipped_iterations);
		}
		if (Timer::elapsedSeconds() > _params.jobWallclockLimit() - TIMEBUFFER_FOR_FINAL_SUBSTITUTE) {
			decide_terminate_job=true;
			LOGGER(_sweeplogger,V2_INFO, "SWEEP [%i](root-trf) TERMINATE due to timeout (%f), with buffer time %f for final substitute \n", _my_rank, _params.jobWallclockLimit(), TIMEBUFFER_FOR_FINAL_SUBSTITUTE);
		}
		//A round is finished if all sweepers are idle or if we didnt have enough progress
		if (decide_end_iteration || decide_terminate_job) {
			LOGGER(_sweeplogger,V2_INFO, "SWEEP [%i](root-trf) (%i)all_idle  (%i)end_iteration (%i)foundUn-sat (%i)terminate_job \n", _my_rank, all_idle, decide_end_iteration, foundUnsat, decide_terminate_job);
			LOGGER(_sweeplogger,V2_INFO, "SWEEP [%i](root-trf) ITERATION %i/%i FINISHED in sharing round %i \n", _my_rank, _root_sweep_iteration, _params.sweepMaxIterations(), _root_sharing_round);
			LOGGER(_sweeplogger,V2_INFO, "SWEEP [%i](root-trf) ITERATION %i/%i shared: %i EQS, %i UNITS  \n", _my_rank, _root_sweep_iteration, _params.sweepMaxIterations(), _root_shared_eqs_this_iteration, _root_shared_units_this_iteration);
			if (_root_sweep_iteration == _params.sweepMaxIterations()) {
				LOGGER(_sweeplogger,V2_INFO, "SWEEP [%i](root-trf): Job finished! All iterations done (%i/%i). Broadcasting termination signal with sharing data.\n", _my_rank, _root_sweep_iteration, _params.sweepMaxIterations());
				decide_terminate_job = true;
			}
			else {
				_root_did_just_finish_iteration = true; //remember for the next round
				_root_initwork_startedproviding = false; //providing work to the solvers can take some time, track that progress
				_root_initwork_provided		= false;
				LOGGER(_sweeplogger,V2_INFO, "SWEEP [%i](root-trf) Preparing for new iteration \n", _my_rank);
			}
		}
		//The root node (and only the root node) tracks the number of completed sweep rounds,
		//and broadcasts this information. This way, also nodes that join later know which round we are in.
		payload[payload.size() - METADATA_SWEEP_ITERATION] = _root_sweep_iteration;
		payload[payload.size() - METADATA_SHARING_ROUND] = _root_sharing_round;
		payload[payload.size() - METADATA_END_ITERATION] = decide_end_iteration;
		payload[payload.size() - METADATA_TERMINATE] = decide_terminate_job;

		//Send my units and equivalences via cross-job communication to the SAT job
		if (!foundUnsat && _clause_comm && _params.crossJobCommunication()) {
			assert(_clause_comm || log_return_false("Sweep ERROR: _clause_comm object missing\n"));
			BufferBuilder bb(-1, 10, false);
			if (_params.sweepXTCSsend()) {
				//Payload Format: [eqs, units, metadata]
				//Read units, which are stored directly after the equivalences
				for (int i=eq_size; i<eq_size+n_sweep_units; i++) {
					int unit = payload[i];
					bb.append({&unit, 1, 1});
				}
				//Read equivalences (need to append to the buffer after units, because they have a larger clause length)
				for (int i=0; i < eq_size; i+=2) {
					int elit1 = payload[i];
					int elit2 = payload[i+1];
					//Represent the equality elit1==elit2 in CNF format via two binary clauses
					int cnfA[2] = {-elit1, elit2};
					int cnfB[2] = {-elit2, elit1};
					bb.append({&cnfA[0],2,2});
					bb.append({&cnfB[0],2,2});
				}
			}
			auto buffer = bb.extractBuffer();
			if (_params.sweepXTCSsend()) {
				LOGGER(_sweeplogger,V4_VVER, "SWEEPsns to XTCS: s %i cl %i \n", buffer.size(), n_eqs*2 + n_sweep_units);
			}
			_clause_comm->feedLocalClausesIntoCrossSharing(buffer, nullptr);

			_clause_comm->communicate();
			// }
			while (hasDeferredMessage()) {
				auto deferredMsg = getDeferredMessage();
				_clause_comm->handle(
					deferredMsg.source, deferredMsg.mpiTag, deferredMsg.msg);
			}
			_crossjob_has_prepared_sharing = true;
		}

		//Within SweepJob, pass down the units received via Cross-Job-Communication to all sweepers.
		//(we use SweepJobs own broadcasting, instead of relying on the CJC broadcasting)
		int crossjob_units_received = 0;
		if (_params.crossJobCommunication() && _params.sweepXTCSrecv()) {
			std::lock_guard<std::mutex> lock(_crossjob_import_mutex);
			if (!_crossjob_root_received_units.empty()) {
				const int insert_pos = eq_size + n_sweep_units;
				// Splice the cross-job units into the existing vector,
				// appending them after the sweep units, but before before the metadata
				payload.insert(
					payload.begin() + insert_pos,
					_crossjob_root_received_units.begin(),
					_crossjob_root_received_units.end()
				);
				crossjob_units_received = static_cast<int>(_crossjob_root_received_units.size());
				assert(payload.size() == eq_size + n_sweep_units + crossjob_units_received + NUM_METADATA_FIELDS);
				const int total_units = n_sweep_units + crossjob_units_received;

				//updated stored unit count to reflect the additions.
				//Otherwise, the sweepers would not know that we added new units
				payload[payload.size() - METADATA_UNIT_SIZE] = total_units;

				//discard the temporary buffer, to not import the same units a second time
				_crossjob_root_received_units.clear();
			}
		}

		char logmsg[512];
		snprintf(logmsg, sizeof(logmsg),
			"SWEEP [%i](root-trf) send: act,idl,lti %i,%i,%i  mxkit %i  iter %i rnd %i :  %i ai  %i endi %i trm  E %i  U %i  XJU %i  SW %i  ST %i  Sched, Swept  %.2f , %.2f °/.  wsucc  %.6f  ETI %i  UTI %i\n",
			_my_rank, active_count, idle_count, longtermidle_count, maxxed_kittens,  _root_sweep_iteration, _root_sharing_round,
			all_idle,  decide_end_iteration, decide_terminate_job, n_eqs, n_sweep_units, crossjob_units_received,
			work_sweeps, work_stepovers,
			done_scheduled_prcnt , 100*(work_sweeps + work_unsched_resweeps)/(double)_numVars, success_in_window, _root_shared_eqs_this_iteration, _root_shared_units_this_iteration
		);
		LOGGER(_sweeplogger, V3_VERB, "%s", logmsg);
		//no return statement, because the payload was just transformed in-place
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
	friend int  cb_custom_query(void *SweeJobState, int query);
	friend void cb_report_iteration(void *SweepJobState, int localId);


private:
	KissatPtr createNewSweeper(int localId);

	void createAndStartNewSweeper(int localId);
    void loadFormula(KissatPtr sweeper);

	void checkSharingDelay();
	void checkForUnsatResults();
	void rootReportSolverResult(KissatPtr sweeper, int res);
	void reportEndStats(KissatPtr sweeper);
	void tryReportToMallob();
	bool checkCrossCommNeedsAdvancing(const std::string &from);
	void reportStealLatencies(KissatPtr sweeper);
	void triggerTerminations();

	bool skip_MPI_forNow();

	void solverGoStealing(KissatPtr sweeper);
	void sendWorkstealsViaMPI();
	void checkIdleWorkStatus();
	void checkForStuckSolvers();

    void rootStartNewSharingRound();
    void cbContributeToAllReduce();
    static std::vector<int> aggregateEqUnitContributions(std::list<std::vector<int>> &contribs);
	static void appendMetadataToReductionElement(int foundUnsat, std::vector<int> &contrib, int idle_count, int longtermidle_count, int active_count, int unit_size, int eq_size, int work_sweeps, int work_stepovers, int unsched_resweeps, int maxxed_kittens);
	void advanceAllReduction();
	void extractAllReductionResult();

	void crossjob_rootReceiveClauses(std::vector<int>  &&clauses);

	std::vector<int> getRandomIdPermutation();
	void printActiveMPIRequestsCount();

	bool canSolverExitStealing(KissatPtr sweeper);
	bool tryProvideInitialWork(KissatPtr sweeper);
	std::vector<int> stealWorkFromAnyLocalSolver(int asking_rank, int asking_sourceLocalId); //parameters only for verbose logging
    std::vector<int> stealWorkFromSpecificLocalSolver(int localId);
	void cbStealWorkNew(unsigned **work, int *work_size, int localId);
	void cbImportEq(int *elit1, int *elit2, int localId);
	void cbImportUnit(int *lit, int localId);
	int  cbCustomQuery(int query);
	void cbReportIteration(int localId);
	void clearImportedRound();

	virtual ~SweepJob();

	//stubs
	bool isInitialized() override {
		LOGGER(_sweeplogger,V1_WARN, "[SweepJob] Called stub: isInitialized\n");
		return true;
	}

	void prepareSharing() override {
		LOGGER(_sweeplogger,V1_WARN, "[SweepJob] Called stub: prepareSharing\n");
		//we prepare sharing anyway in every sharing round, don't need this reminder/callback(?)
	}

	bool hasPreparedSharing() override {

	bool answer = _crossjob_has_prepared_sharing;
	LOGGER(_sweeplogger,V4_VVER, "[SweepJob] Called stub: hasPreparedSharing. return %i  (root)\n",  answer);
	return answer;
	}


	std::vector<int> getPreparedClauses(Checksum& checksum, int& successfulSolverId, int& numLits) override {
		successfulSolverId = -1;
		numLits = 0;
		LOGGER(_sweeplogger,V1_WARN, "[SweepJob] Called stub: getPreparedClauses. return succSolver -1 , numLits 0, vector {}\n");
		_crossjob_has_prepared_sharing = false; //mirroring the behaviour in inter_job_clause_sharer.hpp
		return {};
	}

	void filterSharing(int, std::vector<int>&&) override {
		LOGGER(_sweeplogger,V1_WARN, "[SweepJob] Called stub: filterSharing\n");
	}

	bool hasFilteredSharing(int) override {
		LOGGER(_sweeplogger,V1_WARN, "[SweepJob] Called stub: hasFilteredSharing. return true\n");
		return true;
	}

	std::vector<int> getLocalFilter(int) override {
		LOGGER(_sweeplogger,V1_WARN, "[SweepJob] Called stub: getLocalFilter. return {}\n");
		return {};
	}

	void applyFilter(int, std::vector<int>&&) override {
		LOGGER(_sweeplogger,V1_WARN, "[SweepJob] Called stub: applyFilter\n");
	}

	void digestSharingWithoutFilter(int epoch, std::vector<int>  &&clauses, bool stateless) override {
		InplaceClauseAggregation(clauses).stripToRawBuffer(); //found by Claude
		//We only receive at the root node, all further distribution is handled by our own SweepApp logic
		if (_is_root) {
			LOGGER(_sweeplogger,V3_VERB, "SWEEP receive XTCS s %i\n",clauses.size());
			crossjob_rootReceiveClauses(std::move(clauses));
		}
	}

	void returnClauses(std::vector<int>&&) override {
		LOGGER(_sweeplogger,V1_WARN, "[SweepJob] Called stub: returnClauses\n");
	}

	void digestHistoricClauses(int, int, std::vector<int>&&) override {
		LOGGER(_sweeplogger,V1_WARN, "[SweepJob] Called stub: digestHistoricClauses\n");
	}

	int getLastAdmittedNumLits() override {
		LOGGER(_sweeplogger,V1_WARN, "[SweepJob] Called stub: getLastAdmittedNumLits. return 0\n");
		return 0;
	}

	long long getBestFoundObjectiveCost() override {
		LOGGER(_sweeplogger,V1_WARN, "[SweepJob] Called stub: getBestFoundObjectiveCost. return 0\n");
		return 0;
	}

	void setClauseBufferRevision(int) override {
		LOGGER(_sweeplogger,V1_WARN, "[SweepJob] Called stub: setClauseBufferRevision\n");
	}

	void updateBestFoundSolutionCost(long long) override {
		LOGGER(_sweeplogger,V1_WARN, "[SweepJob] Called stub: updateBestFoundSolutionCost\n");
	}

};

#endif
