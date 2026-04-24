
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
    int _solved_status{-1}; //final status that gets communicated to Mallob, only touched by the main thread
	int _staged_solved_status{-1}; //can be touched by any thread
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
	std::atomic_bool _flag_started_synchronized_solving{false};
	std::atomic<float> _timestamp_started_synchronized_solving{0};
	std::atomic_bool _flag_terminated_while_synchronizing{false};
	// bool _started_sharedelay_tracking{false};
	int _lastLongtermIdleCount{0};

	//Timing
	float			   _timestamp_start_sweepapp;
	std::vector<float> _timestamp_root_started_bcast;
	std::vector<float> _timestamp_receive_sharing_result;
	std::vector<float> _timestamp_contributed_to_sharing;
	std::vector<float> _duration_appl_communicate;
	bool 			   _logged_full_jobcomm{false};
	float			   _timestamp_last_idleinfo;

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
			nr = _nr;
			//atomic flags are changed only now, after modifying the non-atomics
			is_active = true;
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
	static const int NUM_METADATA_FIELDS = 11;
		//field indices must be unique numbers exactly filling 1..NUM_METADATA_FIELDS !
		static const int METADATA_ENVCOMPLETIONS   = 11;
		static const int METADATA_END_ITERATION		  = 10;
		static const int METADATA_WORK_SWEEPS		 = 9;
		static const int METADATA_WORK_STEPOVERS	= 8;
		static const int METADATA_UNSCHED_RESWEEPS = 7;
		static const int METADATA_TERMINATE		  = 6;
		static const int METADATA_SWEEP_ITERATION= 5;
		static const int METADATA_SHARING_ROUND = 4;
		static const int METADATA_IDLE		   = 3;
		static const int METADATA_UNIT_SIZE	  = 2;
		static const int METADATA_EQ_SIZE    = 1;


	//New Version of Importing, via separated vectors per round
	struct importedRound {
		std::vector<int> eqs{};
		std::vector<int> units{};
	};
	struct finishedCounter {
		std::atomic_int threads_finished_eqs=0;
		std::atomic_int threads_finished_units=0;
	};

	//Equalities and units from sharing are stored once on this rank, and sit there for the local solvers to get picked up
	static constexpr int MAX_IMPORT_ROUNDS = 100 * 2000; //Enough for ca. 2000 seconds (~each round takes >=10ms, i.e. max 100 rounds per second)
	std::vector<importedRound> _imported_data{MAX_IMPORT_ROUNDS};
	std::vector<finishedCounter> _finishedRoundCounters{MAX_IMPORT_ROUNDS}; //technically we store atomics in std::vector, but we only construct once with a fixed size and never push_back or resize, so it compiles and should be fine
	std::vector<int> _iteration_of_round = std::vector<int>(MAX_IMPORT_ROUNDS, -9); //dummy value to distinguish from iteration values >=-1
	//Track which data is already present from sharing
	std::atomic_int _lastImportedRound = 0;
	//After all solvers have picked up the shared data, it can be deleted from this rank
	int _lastClearedRound = 0;


	//Termination. Determined during workstealing, broadcasted via sharing
	std::atomic_bool _terminate_all=false; //termination (on this node) due to sharing consensus that there is no more work

	//Have dedicated files for some important logging types. Mostly to protect them from becoming mangled due to concurrent logging, and for nicer post processing (especially not needing to scan through the main large log file)
	Logger _reslogger;  //most important information about results
	Logger _warnlogger; //some relevant warnings
	Logger _contriblogger; //each rank logs how many Eq+Units it contributed each round

	std::mutex _stealinfo_mutex; //when exporting data from the solver to Mallob, need to lock them when extracting them for global sharing, otherwise the solver threads might continue concurrently pushing new data onto them
	std::vector<std::vector<SweepStealInfo>> _stealinfos_per_solver;
	// Logger _steallogger; //each solver logs each of its steal attempts
	// Logger _rootlogger; //Logging information from the root transformation

	// int _completed_envsizes = 0; //received information from sharing

	//the root node tracks the number of sweep iterations and sharing rounds, distributes this information in the sharing operation
	int _root_shared_units_this_iteration = 0;
	int _root_shared_eqs_this_iteration = 0;
	int _root_total_shared_eqs = 0;
	int _root_total_shared_units = 0;
	int _root_rounds_this_iteration = 0;
	int _root_sweep_iteration = 0;
	int _root_sharing_round = 0;
	int _root_env_completions = 0;
	bool _root_did_just_finish_iteration = true; //Starts with true to immediately start into iteration 1.

	const double EARLYEXIT_RATIO = 0.001;


	//Cross-Job-sharing
	std::unique_ptr<AnytimeSatClauseCommunicator> _clause_comm;
	std::unique_ptr<GenericClauseStore> _clause_store;
	std::vector<int>  _crossjob_root_received_units{};
	std::mutex _crossjob_import_mutex;
	bool _crossjob_has_prepared_sharing{false};



	//The root node (and only the root node) tracks progress over the sharing rounds and sweeping iterations
	//It decides whether sharing should continue or whether it should end (either because the last iteration is reached, or because no progress has been made)
	//It broadcasts this decision to all othe ranks, along with general information about the current iteration and round
	//On a technical level, This information is injected here via an inplace root transform at the end of the sharing aggregation, before broadcasting it
	std::function<void(std::vector<int>&)> _inplace_rootTransform = [&](std::vector<int>& payload) {
		assert(_is_root);
		_root_sharing_round++;
		LOG(V2_INFO, "SWEEP [%i](root-trf) rnd(%i) entered \n", _my_rank, _root_sharing_round);

		//Remember from last sharing round whether now begins a new iteration
		if (_root_did_just_finish_iteration) {
			_root_sweep_iteration++;
			_root_did_just_finish_iteration = false;

			//Only now the new iteration truly begins, so only now we reset these iteration-specific counters
			_root_shared_units_this_iteration = 0;
			_root_shared_eqs_this_iteration = 0;
			_root_rounds_this_iteration=0;

			LOG(V2_INFO, "SWEEP [%i](root-trf) ITERATION %i/%i STARTED \n", _my_rank, _root_sweep_iteration, _params.sweepMaxIterations());
		}

		int n_sweep_units = payload[payload.size() - METADATA_UNIT_SIZE];
		int eq_size  = payload[payload.size() - METADATA_EQ_SIZE];
		int n_eqs	 = eq_size / 2;  //each equivalence takes up two integers
		bool all_idle= payload[payload.size() - METADATA_IDLE];
		int work_sweeps			 = payload[payload.size() - METADATA_WORK_SWEEPS];
		int work_stepovers		 = payload[payload.size() - METADATA_WORK_STEPOVERS];
		int work_unsched_resweeps= payload[payload.size() - METADATA_UNSCHED_RESWEEPS];

		_root_shared_units_this_iteration += n_sweep_units;
		_root_shared_eqs_this_iteration   += n_eqs;
		_root_total_shared_units += n_sweep_units;
		_root_total_shared_eqs   += n_eqs;
		_root_rounds_this_iteration++;

		bool earlyexit = false;
		int swept = work_sweeps + work_unsched_resweeps;
		int eliminated = _root_shared_units_this_iteration + _root_shared_eqs_this_iteration; //slight overestimation, because the same eq can be shared in different rounds. But in the relevant regime (almost no sharing) this is not a problem.
		double progress_ratio = (swept==0 ? 0 : eliminated/(double)swept);
		//mirror the "delay" decision in original kissat sweeping. There, if  not enough progress is made, the next sweeping is delayed - here we exit the iteration early
		if (_params.sweepMinExitSwept()!=0 && swept >= _params.sweepMinExitSwept.val) {
			if (progress_ratio <= EARLYEXIT_RATIO) {
				LOG(V2_INFO, "SWEEP [%i](root-trf) EARLYEXIT in iteration %i, round %i: only %zu / %zu progress \n", _my_rank, _root_sweep_iteration, _root_sharing_round, eliminated, swept);
				earlyexit = true;
			}
		}


		bool send_terminate = false;
		bool send_end_iteration = false;
		//A round is finished if all sweepers are idle or if we didnt have enough progress
		if (all_idle || earlyexit) {
			LOG(V2_INFO, "SWEEP [%i](root-trf) (%i)all_idle  (%i)earlyexit \n", _my_rank, all_idle, earlyexit);
			LOG(V2_INFO, "SWEEP [%i](root-trf) ITERATION %i/%i FINISHED (seen at root transform) in sharing round %i \n", _my_rank, _root_sweep_iteration, _params.sweepMaxIterations(), _root_sharing_round);
			LOG(V2_INFO, "SWEEP [%i](root-trf) ITERATION %i/%i shared: %i EQS, %i UNITS  \n", _my_rank, _root_sweep_iteration, _params.sweepMaxIterations(), _root_shared_eqs_this_iteration, _root_shared_units_this_iteration);
			//The kissat solver will report the sweep stats itself via a callback once it has cleaned up its internal database and metrics via substitute()
			// printSweepStats(_sweepers[_representative_localId], false); //report some intermediate statistics about this iteration
			bool progress = (_root_shared_eqs_this_iteration + _root_shared_units_this_iteration) > 0;
			bool lastsweepround = (_root_sweep_iteration == _params.sweepMaxIterations());
			if (_params.sweepToCompletion()) {
				lastsweepround = false;
			}
			if (lastsweepround || (!progress && _params.sweepTermNoProgress())) {
				if (lastsweepround) LOG(V2_INFO, "SWEEP [%i](root-trf): Job finished! All iterations done (%i/%i). Broadcasting termination signal with sharing data.\n", _my_rank, _root_sweep_iteration, _params.sweepMaxIterations());
				if (!progress && _params.sweepTermNoProgress())	LOG(V2_INFO, "SWEEP [%i](root-trf): Job finished! No more progress in iteration %i/%i. Broadcasting termination signal with sharing data.\n", _my_rank, _root_sweep_iteration, _params.sweepMaxIterations());
				//we DON'T yet set _terminate_all=1 here, because we want also the root solver to first import this last sharing information, which contains valuable equalities and units, before terminating the solvers
				send_end_iteration = true;
				send_terminate = true;
			}
			else {
				_root_did_just_finish_iteration = true;
				_root_initwork_startedproviding = false;
				_root_initwork_provided		= false;
				send_end_iteration = true;
				LOG(V2_INFO, "SWEEP [%i](root-trf) Preparing for new iteration. Have set (%i)started_providing_work, (%i)provided_work  \n", _my_rank, _root_initwork_startedproviding.load(), _root_initwork_provided.load() );
			}
			if (_params.sweepToCompletion() && !progress) {
				_root_env_completions++;
			}
		}

		// _root_env_completions = _root_sweep_iteration/2;

		//The root node (and only the root node) tracks the number of completed sweep rounds, and broadcasts this information. This way, also nodes that join later know which round we are in.
		payload[payload.size() - METADATA_SWEEP_ITERATION] = _root_sweep_iteration;
		payload[payload.size() - METADATA_SHARING_ROUND] = _root_sharing_round;
		payload[payload.size() - METADATA_END_ITERATION] = send_end_iteration;
		payload[payload.size() - METADATA_TERMINATE] = send_terminate;
		payload[payload.size() - METADATA_ENVCOMPLETIONS] = _root_env_completions;
		//the all_idle payload is already set

		//create char to print the same msg in two logs
		//once for completeness chronologically in the general logs, and once in a special smaller file on the root node for easier postprocessing

		//Cross-Job Interaction.
		//Send my units and equivalences to the cross-job sharing
		if (_clause_comm && _params.crossJobCommunication()) {
			assert(_clause_comm || log_return_false("Sweep ERROR: _clause_comm object missing\n"));
			BufferBuilder bb(-1, 10, false);
			//For debugging purposes another flag for actually sending our stuff to cjc
			if (_params.sweepXJsendTo()) {
				//Payload Format: [eqs, units, metadata]
				//Read units, which are stored directly after the equivalences
				for (int i=eq_size; i<eq_size+n_sweep_units; i++) {
					int unit = payload[i];
					bb.append({&unit, 1, 1});
					LOG(V2_INFO, "   sproduce: %i\n", unit);
				}
				//Read equivalences (need to append to the buffer after units, because they have a larger clause length)
				for (int i=0; i < eq_size; i+=2) {
					int elit1 = payload[i];
					int elit2 = payload[i+1];
					//Convert elit1==elit2 in CNF form
					int cnfA[2] = {-elit1, elit2};
					int cnfB[2] = {-elit2, elit1};
					bb.append({&cnfA[0],2,2});
					bb.append({&cnfB[0],2,2});
					// LOG(V2_INFO, "   sproduce: %i %i   &   %i %i\n", -elit1, elit2, -elit2, elit1);
				}
			}
			auto buffer = bb.extractBuffer();
			if (_params.sweepXJsendTo()) {
				LOG(V1_WARN, "snsSweep feed to XTCS size %i  clauses %i \n", buffer.size(), n_eqs*2 + n_sweep_units);
			} else {
				LOG(V1_WARN, "snsSweep feed dummy buffer to Crosssharing: buffersize %i\n", buffer.size());
			}
			_clause_comm->feedLocalClausesIntoCrossSharing(buffer, nullptr);

			//communicate() happens in appl_communicate()
			// if (_clause_comm) {
			_clause_comm->communicate();
			// }
			while (hasDeferredMessage()) {
				auto deferredMsg = getDeferredMessage();
				_clause_comm->handle(
					deferredMsg.source, deferredMsg.mpiTag, deferredMsg.msg);
			}
			_crossjob_has_prepared_sharing = true;
		}


		//Integrate units which were received via Cross-Job Sharing into the sweep-internal sharing vector
		//This way, only the root rank "knows" which units came from sweeping and which from the cross-job sharing
		//All other ranks will just receive a larger sharing vector, without the details which units came from where
		int crossjob_units_added = 0;
		if (_params.crossJobCommunication() && _params.sweepXJrecvFrom()) {
			std::lock_guard<std::mutex> lock(_crossjob_import_mutex);
			if (!_crossjob_root_received_units.empty()) {
				const int insert_pos = eq_size + n_sweep_units;
				// Splice the cross-job units in right after the sweep units, before metadata
				payload.insert(
					payload.begin() + insert_pos,
					_crossjob_root_received_units.begin(),
					_crossjob_root_received_units.end()
				);
				for (int elit : _crossjob_root_received_units) {
					LOG(V2_INFO, "SWEEP [%i](root-trf) insert unit from crossjob: %i \n", _my_rank, elit);
				}
				crossjob_units_added = static_cast<int>(_crossjob_root_received_units.size());
				assert(payload.size() == eq_size + n_sweep_units + crossjob_units_added + NUM_METADATA_FIELDS);
				const int total_units = n_sweep_units + crossjob_units_added;
				//updated stored unit count to reflect the additions.
				payload[payload.size() - METADATA_UNIT_SIZE] = total_units;
				LOG(V2_INFO, "SWEEP [%i](root-trf) spliced cross-job units into payload: %i \n", _my_rank, crossjob_units_added, n_sweep_units);

				//discard the temporary buffer, to not import the same units a second time
				_crossjob_root_received_units.clear();
			}
		}


		char logmsg[512];
		snprintf(logmsg, sizeof(logmsg),
			"SWEEP [%i](root-trf) send: envsize %i iter %i rnd %i :  %i ai  %i endi %i trm  E %i  U %i  XJU %i   SW %i  ST %i  RE %i    Sched, Swept  %i  %i    ( %.2f ,  %.2f )°/.   succ-rate %.6f   \n",
			_my_rank, _root_env_completions, _root_sweep_iteration, _root_sharing_round, all_idle,  send_end_iteration, send_terminate, n_eqs, n_sweep_units, crossjob_units_added,
			work_sweeps, work_stepovers, work_unsched_resweeps, work_sweeps + work_stepovers, work_sweeps + work_unsched_resweeps,
			100*(work_sweeps + work_stepovers)/(double)_numVars , 100*(work_sweeps + work_unsched_resweeps)/(double)_numVars,
			progress_ratio
		);
		LOG(			   V2_INFO, "         %s", logmsg);
		LOGGER(_reslogger, V2_INFO, "%s", logmsg);
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

    // bool appl_isDestructible() override {
		// SAT comm. present which is not destructible (yet)?
		// if (_clause_comm && !_clause_comm->isDestructible()) {
			// for (int i = 0; i < 10; i++) _clause_comm->communicate(); // may advance destructibility
			// return false;
		// }
		// TODO(Nicco) Did you not at some point implement isDestructible for this job?
		// return true;
	// }

    int appl_solved() override {
		// TODO(Nicco) Before reporting a result via the below line, check via
		//_clause_comm->hasLocalClausesLeftToShare();
		// (should be from the main thread) if there's still some clauses that need to be shared.
		// In that case, appl_communicate() needs to call this from time to time:
		// _clause_comm->feedLocalClausesIntoCrossSharing(buffer, nullptr);
		// (with an empty buffer from a BufferBuilder) since this will initiate XTCS operations.
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
    // void advanceSweepMessage(JobMessage& msg);
	KissatPtr createNewSweeper(int localId);

	void createAndStartNewSweeper(int localId);
    void loadFormula(KissatPtr sweeper);

	void checkSharingDelay();
	void checkForUnsatResults();
	void rootReportSolverResult(KissatPtr sweeper, int res);
	void reportEndStats(KissatPtr sweeper);
	void tryReportToMallob();
	bool checkCrossCommNeedsAdvancing(const std::string &from);
	void saveStealLatencies(KissatPtr sweeper);
	void triggerTerminations();

	bool skip_MPI_forNow();

	void solverGoStealing(KissatPtr sweeper);
	void sendWorkstealsViaMPI();
	void printIdleWorkStatus();

    void rootStartNewSharingRound();
    void cbContributeToAllReduce();
    static std::vector<int> aggregateEqUnitContributions(std::list<std::vector<int>> &contribs);
	static void appendMetadataToReductionElement(std::vector<int> &contrib, int is_idle, int unit_size, int eq_size, int work_sweeps, int work_stepovers, int unsched_resweeps);
	void advanceAllReduction();
	void extractAllReductionResult();

	void crossjob_rootReceiveClauses(std::vector<int>  &&clauses);

	std::vector<int> getRandomIdPermutation();

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
		LOG(V1_WARN, "[SweepJob] Called stub: isInitialized\n");
		return true;
	}

	void prepareSharing() override {
		LOG(V1_WARN, "[SweepJob] Called stub: prepareSharing\n");
		//we prepare sharing anyway in every sharing round, don't need this reminder/callback(?)
	}

	bool hasPreparedSharing() override {

		//for now only consider case with one rank, where it is automatically root
		//with 2 ranks new problems arrived..., since we only really feed at the root node?

		// if (_is_root) {
			//seems that at some point we need to return true, otherwise this gets called indefinitely at the end of this job...
			bool answer = _crossjob_has_prepared_sharing;
			// if (_terminate_all || _staged_solved_status!=-1) {
			// answer = true;
			// }
			LOG(V1_WARN, "[SweepJob] Called stub: hasPreparedSharing. return %i  (root)\n",  answer);
			return answer;
		// } else {
			// BufferBuilder bb(_clause_store->getBufferBuilder(-1));
			// auto buffer = bb.extractBuffer();
			// _clause_comm->feedLocalClausesIntoCrossSharing(buffer, nullptr);
			// LOG(V1_WARN, "[SweepJob] Called stub: hasPreparedSharing. return 1  (dummy buffer, non-root)\n");
			// return true;
		// }
	}


	std::vector<int> getPreparedClauses(Checksum& checksum, int& successfulSolverId, int& numLits) override {
		successfulSolverId = -1;
		numLits = 0;
		LOG(V1_WARN, "[SweepJob] Called stub: getPreparedClauses. return succSolver -1 , numLits 0, vector {}\n");
		_crossjob_has_prepared_sharing = false; //mirroring the behaviour in inter_job_clause_sharer.hpp
		return {};
	}

	void filterSharing(int, std::vector<int>&&) override {
		LOG(V1_WARN, "[SweepJob] Called stub: filterSharing\n");
	}

	bool hasFilteredSharing(int) override {
		LOG(V1_WARN, "[SweepJob] Called stub: hasFilteredSharing. return true\n");
		return true;
	}

	std::vector<int> getLocalFilter(int) override {
		LOG(V1_WARN, "[SweepJob] Called stub: getLocalFilter. return {}\n");
		return {};
	}

	void applyFilter(int, std::vector<int>&&) override {
		LOG(V1_WARN, "[SweepJob] Called stub: applyFilter\n");
	}

	void digestSharingWithoutFilter(int epoch, std::vector<int>  &&clauses, bool stateless) override {
		LOG(V1_WARN, "SWEEP receive XTCS size %i\n",clauses.size());
		if (_is_root) {
			crossjob_rootReceiveClauses(std::move(clauses));
		}
	}

	void returnClauses(std::vector<int>&&) override {
		LOG(V1_WARN, "[SweepJob] Called stub: returnClauses\n");
	}

	void digestHistoricClauses(int, int, std::vector<int>&&) override {
		LOG(V1_WARN, "[SweepJob] Called stub: digestHistoricClauses\n");
	}

	int getLastAdmittedNumLits() override {
		LOG(V1_WARN, "[SweepJob] Called stub: getLastAdmittedNumLits. return 0\n");
		return 0;
	}

	long long getBestFoundObjectiveCost() override {
		LOG(V1_WARN, "[SweepJob] Called stub: getBestFoundObjectiveCost. return 0\n");
		return 0;
	}

	void setClauseBufferRevision(int) override {
		LOG(V1_WARN, "[SweepJob] Called stub: setClauseBufferRevision\n");
	}

	void updateBestFoundSolutionCost(long long) override {
		LOG(V1_WARN, "[SweepJob] Called stub: updateBestFoundSolutionCost\n");
	}


};

#endif
