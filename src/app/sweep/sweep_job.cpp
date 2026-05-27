#include "sweep_job.hpp"

#include <memory>
#include <algorithm>
#include <sys/wait.h>

#include "app/job.hpp"
#include "app/job_tree.hpp"
#include "app/sat/job/anytime_sat_clause_communicator.hpp"
#include "app/sat/job/base_sat_job.hpp"
#include "app/sat/job/clause_sharing_session.hpp"
#include "app/sat/sharing/sharing_manager.hpp"
#include "app/sat/sharing/buffer/buffer_builder.hpp"
#include "app/sat/sharing/store/static_clause_store_by_lbd.hpp"
#include "app/sat/sharing/store/static_clause_store_mixed_lbd.hpp"
#include "util/ctre.hpp"
#include "util/logger.hpp"
#include "util/sys/tmpdir.hpp"


extern "C" {
#include "kissat/src/kissat.h"
}



SweepJob::SweepJob(const Parameters& params, const JobSetup& setup, AppMessageTable& table)
    : BaseSatJob(params, setup, table),
	_sweeplogger(Logger::getMainInstance().copy("<SWEEP>", ".sweep"))
{
	assert(_params.jobCommUpdatePeriod() > 0 || log_return_false("[ERROR] For this application to work,"
            " you must explicitly enable job communicators with the -jcup option, e.g., -jcup=0.1\n"));

	LOG(V2_INFO, "## \n");
	LOG(                 V2_INFO, "New SweepJob MPI Process on rank [%i] with %i threads, ctx %i \n", getJobTree().getRank(), params.numThreadsPerProcess.val, getJobTree().getContextId());
	LOGGER(_sweeplogger, V2_INFO, "New SweepJob MPI Process on rank [%i] with %i threads, ctx %i \n", getJobTree().getRank(), params.numThreadsPerProcess.val, getJobTree().getContextId());
	LOG(V2_INFO, "## \n");
}


//callback from kissat
void cb_search_work_in_tree(void *SweepJob_state, unsigned **work, int *work_size, int local_id) {
    ((SweepJob*) SweepJob_state)->cbStealWorkNew(work, work_size, local_id);
}

void cb_import_eq(void *SweepJobState, int *elit1, int *elit2, int localId) {
	((SweepJob*) SweepJobState)->cbImportEq(elit1, elit2, localId);
}

void cb_import_unit(void *SweepJobState, int *elit, int localId) {
	((SweepJob*) SweepJobState)->cbImportUnit(elit, localId);
}

int cb_custom_query(void *SweepJobState, int localId, int query) {
	return ((SweepJob*)SweepJobState)->cbCustomQuery(localId, query);
}

void cb_report_iteration(void *SweepJobState, int localId) {
	return ((SweepJob*)SweepJobState)->cbReportIteration(localId);
}

void SweepJob::appl_start() {
	if (_params.sweepMaxIterations.val==0) {
		LOGGER(_sweeplogger,V2_INFO,"Skip SWEEP JOB, as sweepMaxIterations==0");
		return;
	}
	_started_appl_start = true;
	_my_rank = getJobTree().getRank();
	_my_index = getJobTree().getIndex();
	_my_ctx_id = getJobTree().getContextId();
	_is_root = getJobTree().isRoot();
	_nThreads = min( getNumThreads(), _params.numThreadsPerProcess.val); //done in constructor
	if (_nThreads < _params.numThreadsPerProcess.val) {
		LOGGER(_sweeplogger,V1_WARN,"SWEEP WARN : cut down threads to %i \n", _nThreads);
	}
	const JobDescription& desc = getDescription();
	int numVars = desc.getAppConfiguration().fixedSizeEntryToInt("__NV");
	int numClauses = desc.getAppConfiguration().fixedSizeEntryToInt("__NC");

	LOGGER(_sweeplogger,V2_INFO,"SWEEP JOB SweepJob appl_start() STARTED: Rank %i, Index %i, ContextId %i, is root? %i, Parent-Rank %i, Parent-Index %i, threads=%d, NumVars %i, NumClauses %i\n",
		_my_rank, _my_index, getJobTree().getContextId(), _is_root, getJobTree().getParentNodeRank(), getJobTree().getParentIndex(), _nThreads, numVars, numClauses);
	LOG(                V2_INFO,"SWEEP JOB SweepJob appl_start() STARTED: Rank %i, Index %i, ContextId %i, is root? %i, Parent-Rank %i, Parent-Index %i, threads=%d, NumVars %i, NumClauses %i\n",
		_my_rank, _my_index, getJobTree().getContextId(), _is_root, getJobTree().getParentNodeRank(), getJobTree().getParentIndex(), _nThreads, numVars, numClauses);
	LOGGER(_sweeplogger,V2_INFO,"SWEEP_PAYLOAD_SIZE %i\n", getDescription().getFormulaPayloadSize(0));
	LOGGER(_sweeplogger,V2_INFO,"SWEEP_NUM_VARS %i\n", numVars);
	LOGGER(_sweeplogger,V2_INFO,"SWEEP_NUM_CLAUSES %i\n", numClauses);
	for (auto& group : _params._grouped_list) {
		  if (group->groupId != "app/sweep") continue;
		  std::map<std::string, Option*> sorted;
		  for (const auto& [id, opt] : group->map) sorted[id] = opt;
		  for (const auto& [id, opt] : sorted) {
			  LOGGER(_sweeplogger, V2_INFO, "SWEEPAPP_OPTION %s  %-22s = %s\n",
					 opt->id.c_str(), opt->longid.c_str(), opt->getValAsString().c_str());
		  }
		  break;
	}

	_nThreads = _params.numThreadsPerProcess.val;
    _metadata = getSerializedDescription(0)->data();
	_timestamp_start_sweepapp = Timer::elapsedSeconds();
	_worksteal_requests.resize(_nThreads);

	//To randomize workstealing on a given rank, we create a list of all ids that will be then shuffled each time
	std::ostringstream oss;
	for (int localId=0; localId < _nThreads; localId++) {
		_list_of_ids.push_back(localId);
		oss << localId << ",";
	}
	LOGGER(_sweeplogger, V3_VERB,"LIST_OF_LOCAL_IDS: %s \n", oss.str().c_str());

	//Will hold pointers to the kissat solvers
	_sweepers.resize(_nThreads);

	//Initialize the background workers, each will run one kissat thread
	_bg_workers.reserve(_nThreads);
	for (int i = 0; i < _nThreads; ++i) {
		_bg_workers.emplace_back(std::make_unique<BackgroundWorker>());
	}

	_bcast.reset(new JobTreeBroadcast(getId(), getJobTree().getSnapshot(), [this]() {cbContributeToAllReduce();}, TAG_BCAST_INIT));
	_red.reset();

	//Start individual Kissat threads (those then immediately jump into the sweep algorithm)
	LOGGER(_sweeplogger, V3_VERB,"Create solvers\n");
	for (int localId=0; localId < _nThreads; localId++) {
		createAndStartNewSweeper(localId);
	}

	if (_params.crossJobCommunication()) {
        _clause_comm = std::make_unique<AnytimeSatClauseCommunicator>(_params, this, false);
	}

	//set some general metadata information
	_internal_result.id = getId();
	_internal_result.revision = getRevision();

	LOGGER(_sweeplogger, V3_VERB, "SWEEP appl_start() FINISHED\n");
	//fmcad commit
}



// Called periodically by the main thread to allow the worker to emit messages. Must exit quickly.
void SweepJob::appl_communicate() {
	LOGGER(_sweeplogger, V5_DEBG, "SWEEP appl_communicate() \n");
	float t0 = Timer::elapsedSeconds();
	if (_bcast && _is_root && !_terminate_all.load(std::memory_order_relaxed)) {
		_bcast->updateJobTree(getJobTree());
	}
	if (_bcast && _is_root)// Root: Update job tree snapshot in case your children changed
		_bcast->updateJobTree(getJobTree());

	if (!_logged_full_jobcomm && getJobComm().size() == getVolume()) {
		_logged_full_jobcomm = true;
		LOGGER(_sweeplogger, V3_VERB, "SWEEP FULL-JOBCOMM , all ranks are online now \n");
	}

	rootStartNewSharingRound();
	advanceAllReduction(); //contains pushing data to Cross-Job-Sharing in root transformation
	sendWorkstealsViaMPI();

	if (_clause_comm) {
		_clause_comm->communicate();
		//triggers digestSharingWithoutFilter(...) on this rank, where we receive the shared data from the other jobs
		while (hasDeferredMessage()) {
			LOGGER(_sweeplogger, V3_VERB, "clause_comm has deferred message\n");
			auto deferredMsg = getDeferredMessage();
			_clause_comm->handle(deferredMsg.source, deferredMsg.mpiTag, deferredMsg.msg);
		}
	}
	checkSharingDelay();
	checkIdleWorkStatus();
	checkForUnsatResults();

	clearImportedRound();
	checkCrossCommNeedsAdvancing("appl_communicate");
	tryReportToMallob();

	float t1 = Timer::elapsedSeconds();
	if (t1-t0 > _max_appl_comm_duration) {
		_max_appl_comm_duration = t1-t0;
	}
	LOGGER(_sweeplogger, V5_DEBG, "appl_communicate(): %.6f sec \n", (t1-t0));
	LOGGER(_sweeplogger, V5_DEBG, "appl_communicate() done \n");
}


void SweepJob::createAndStartNewSweeper(int localId) {
	LOGGER(_sweeplogger,V4_VVER, "SWEEP JOB [%i](%i) queuing background worker thread\n", _my_rank, localId);
	_bg_workers[localId]->run([this, localId]() {
		LOGGER(_sweeplogger,V3_VERB, "SWEEP JOB [%i](%i) WORKER START \n", _my_rank, localId);

		auto sweeper = createNewSweeper(localId);

		loadFormula(sweeper);
		_started_sweepers_count++; //only additive, monotonically increasing, going from 0...nThreads-1 and never decreases
		_running_sweepers_count++; //tracks actual number of running solvers at any given moment in time
		/*
		 *  Synchronization Layer!
		 *  We wait here until all other solvers are also initialized, and only then start solving
		 *  This is relevant for sweeping quality, as otherwise the solvers joining late might miss some of the equalities shared in the first rounds
		 *  Alternatively, store all the shared information as a warmup-greeting-package for newly joining solvers, to maximize quality. But only relevant if the SweepJob grows with time, which is not currently the case.
		 */
		while (_started_sweepers_count < _nThreads) {
			LOGGER(_sweeplogger,V5_DEBG, "SWEEP [%i](%i) waits for other solvers (started %i/%i)\n", _my_rank, localId, _started_sweepers_count.load(), _nThreads);
			usleep(500); //2ms
			if (_terminate_all) {
				break;
			}
		}

		if (_terminate_all) {
			LOGGER(_sweeplogger,V3_VERB, "SWEEP [%i](%i): terminated while waiting in synchronization \n", _my_rank, localId);
			_running_sweepers_count--;
			_finished_sweepers_count++; //only monotonically increasing
			_flag_terminated_while_synchronizing = true;
			return;
		}

		LOGGER(_sweeplogger,V3_VERB, "SWEEP JOB [%i](%i) solve() START \n", _my_rank, localId);

		//only now expose the solver to the rest of the system, now that solving starts
		_sweepers[localId] = sweeper;
		_flag_started_synchronized_solving = true;
		_timestamp_started_synchronized_solving = Timer::elapsedSeconds();
		shweep_set_wallclock_offset(sweeper->solver, -1.0 * Timer::elapsedSeconds());

		LOGGER(_sweeplogger, V3_VERB, "SWEEP [%i](%i) START solve() \n", _my_rank, localId);
		int res = sweeper->solve(0, nullptr);
		LOGGER(_sweeplogger, V3_VERB, "SWEEP [%i](%i) FINISH solve(). Result %i \n", _my_rank, localId, res);


		if (res==UNSAT) {
			//Found UNSAT
			assert(kissat_is_inconsistent(sweeper->solver) || log_return_false("SWEEP ERROR: Solver returned UNSAT 20 but is not in inconsistent (==UNSAT) state!\n"));
			LOGGER(_sweeplogger,V4_VVER, "SWEEP [%i](%i) found UNSAT! \n", _my_rank, localId);
			if (_is_root) {
				//for consistency, only the root node is allowed to report to Mallob
				rootReportSolverResult(nullptr, UNSAT);
			} else {
				//if we are not on root, this flag lets the main Process soon send an MPI message to root, indicating UNSAT
				_do_report_UNSAT_to_root = true;
			}
			//To reduce concurrency problems, only a single representative solver on the root node is allowed to report to Mallob
		} else if (res==UNKNOWN && _is_root && sweeper->getLocalId()==_representative_localId) {
			//Found either IMPROVED or UNKNOWN
			auto stats = sweeper->fetchSweepStats();
			if (stats.clauses < stats.start_clauses) {
				//Found some improvements
				rootReportSolverResult(sweeper, IMPROVED);
			} else {
				//whole sweeping didn't yield any improvements
				rootReportSolverResult(sweeper, UNKNOWN);
			}
		}

		assert(res==UNKNOWN || res==UNSAT || log_return_false("SWEEP ERROR: solver has returned with unexpected signal %i \n", res));

		//A dedicated solver on the root node prints his stats as a representative of all other solvers.
		//stats differ slightly between solvers, but especially these global stats are very similar
		//between all of them, so we don't bother aggregating/averaging them
		if (_is_root && localId == _representative_localId) {
			reportEndStats(sweeper);
		}
		reportStealLatencies(sweeper);

		if (_running_sweepers_count==1) {
			LOGGER(_sweeplogger, V2_INFO, "RANK_CONTRIBUTED_UNITS %i\n", _rank_contributed_units);
			LOGGER(_sweeplogger, V2_INFO, "RANK_CONTRIBUTED_EQS %i\n", _rank_contributed_equalities);
		}
		//job returns by default UNKNOWN if no solver has set UNSAT or IMPROVED until now

		//write kissat timing profile
		_sweepers[localId]->cleanUp();
		//this should delete the only persistent shared pointer on the solver, and thus trigger its destructor soon
		_sweepers[localId].reset();
		_running_sweepers_count--;
		_finished_sweepers_count++;
		LOGGER(_sweeplogger,	V3_VERB, "SWEEP [%i](%i) WORKER EXITED. still running: %i\n", _my_rank, localId, _running_sweepers_count.load());
		printActiveMPIRequestsCount();
	});
}


std::shared_ptr<Kissat> SweepJob::createNewSweeper(int localId) {
	const JobDescription& desc = getDescription();
	SolverSetup setup;
	setup.logger = &Logger::getMainInstance();
	setup.jobname = "sweep-"+to_string(_my_index);
	setup.numVars = desc.getAppConfiguration().fixedSizeEntryToInt("__NV");
	setup.numOriginalClauses = desc.getAppConfiguration().fixedSizeEntryToInt("__NC");
	setup.localId = localId;
	setup.globalId = _my_rank * _nThreads + localId;

	if (_params.satProfilingLevel() >= 0) {
		setup.profilingBaseDir = _params.satProfilingDir();
		if (setup.profilingBaseDir.empty()) setup.profilingBaseDir = TmpDir::getGeneralTmpDir();
		setup.profilingBaseDir += "/" + std::to_string(_my_rank) + "/";
		FileUtils::mkdir(setup.profilingBaseDir);
		setup.profilingLevel = _params.satProfilingLevel();
	}

	if (_numVars==0)
		_numVars = setup.numVars;

	float t0 = Timer::elapsedSeconds();
	auto sweeper = std::make_shared<Kissat>(setup);
	float t1 = Timer::elapsedSeconds();
	float init_dur =  (t1 - t0);
	//Usual kissat initializations take 0.2ms in the Sat Solver Subprocess
	//and 4-25ms  in the sweep job (for some weird reasons), but should never be above ~30ms, we warn at >50ms
	const float WARN_init_dur = 0.050;
	LOGGER(_sweeplogger, V3_VERB, "SWEEP [%i](%i) kissat init %.6f sec (%i/%i started)\n", _my_rank, localId, init_dur, _started_sweepers_count.load(), _nThreads);
	if (init_dur > WARN_init_dur) {
		LOGGER(_sweeplogger,V1_WARN, "SWEEP WARN STARTUP [%i](%i): kissat init took unusually long, %.6f sec !\n", _my_rank, localId, init_dur);
	}

	sweeper->setToSweeper();

	//Connecting kissat to Kissat
	sweeper->sweepSetExportCallbacks();

	//Connecting kissat directly to SweepJob
    shweep_set_search_work_callback(sweeper->solver, this, cb_search_work_in_tree);
	shweep_set_SweepJob_eq_import_callback(sweeper->solver, this, cb_import_eq);
	shweep_set_SweepJob_unit_import_callback(sweeper->solver, this, cb_import_unit);

	if (_is_root) {
		//we want to read out the final formula at the root node for convenience,
		//so we provide this callback only to root-node solvers in the first place
		sweeper->sweepSetFormulaReportCallback();
		sweeper->setRepresentativeLocalId(_representative_localId);
		//One representive solver at the root node reports about its new kissat-internal state
		//after each iteration (e.g., number of active variables, clauses, etc)
		if (localId==_representative_localId) {
			shweep_set_report_finished_iteration_callback(sweeper->solver, this, cb_report_iteration);
		}
	}


    //Basic configuration
    sweeper->set_option("quiet", _params.sweepSolverQuiet());  //suppress any standard kissat messages
    sweeper->set_option("verbose", 0);//the native kissat verbosity
    sweeper->set_option("log", 0);    //potentially extensive logging
    sweeper->set_option("check", 0);  //do not check model or derived clauses, because we anyways dont have proof tracking
    sweeper->set_option("statistics", 1);  //print full statistics
    sweeper->set_option("profile", max(_params.satProfilingLevel.val, 0)); //detailed profiling. kissat allows down to 0, mallob down to -1
	sweeper->set_option("seed", 0);   //Sweeping should not contain any own RNG part

	//Specific due to Mallob
	sweeper->set_option("mallob_sweeping", 1); //Bypasses all other Kissat-stuff and goes directly to MallobSweep Logic
	sweeper->set_option("mallob_custom_sweep_verbosity", _params.sweepSolverVerbosity.val); //Sweeper verbosity 0..4
	sweeper->set_option("mallob_local_id", localId);
	sweeper->set_option("mallob_rank", _my_rank);
	sweeper->set_option("mallob_is_root", _is_root);
	sweeper->set_option("mallob_resweep_chance", _params.sweepResweepChance.val);
	sweeper->set_option("mallob_staggered_logs", 1); //set to 1 to have spatially separated logs, useful for verbose runs with 2-16 threads
	sweeper->set_option("mallob_initial_congruence", _params.sweepInitialCongruence.val);
	sweeper->set_option("mallob_signal_kitten", _params.sweepSignalKitten());

	//Own options of Kissat
	sweeper->set_option("sweepcomplete", 1); //deactivates checking for time limits during sweeping, so we dont get kicked out due to some limits
	//Start already with depth 3, and accordingly doubled sweepvars and clauses than the default
  	sweeper->set_option("sweepdepth", 3);				//, 2,    0, INT_MAX,	"environment depth")
  	sweeper->set_option("sweepvars", 256*2);			//  256,  0, INT_MAX,	"environment variables")
  	sweeper->set_option("sweepclauses", 1024*2);		//	1024, 0, INT_MAX,	"environment clauses")
  	sweeper->set_option("sweepmaxdepth", _params.sweepMaxDepth.val); //	//	3,    1, INT_MAX,	"maximum environment depth")
  	sweeper->set_option("sweepmaxvars", 64 * 8192);		//	8192, 2, INT_MAX,	"maximum environment variables")
  	sweeper->set_option("sweepmaxclauses", 64 * 32768);	//	32768,2, INT_MAX,	"maximum environment clauses")
  	sweeper->set_option("sweepfliprounds", 1);		//	1,    0, INT_MAX,	"flipping rounds")
  	sweeper->set_option("sweeprand", 0);			//  0,    0,    1,		"randomize sweeping environment")
  	sweeper->set_option("puresweep_maxKittenProp", _params.sweepMaxKittenProp()); //limit Kitten SAT calls

	sweeper->set_option("substitute", 1);	   //apply equivalence substitutions after sweeping, keep here explicitly to remember it
	sweeper->set_option("substituterounds", 2);//there does not seem to be any need to go higher, almost always all equivalences are already found in the very first round

	sweeper->set_option("preprocess", 0); //skip this part in search.c
	sweeper->set_option("luckyearly", 0); //skip this part in search.c
	sweeper->set_option("luckylate", 0);  //skip this part in search.c
	sweeper->interruptionInitialized = true;
	return sweeper;
}


void SweepJob::tryReportToMallob() {
	//needs to be called from the main thread
	if (!_is_root) {
		return;
	}
	if (_staged_solved_status==-1) {
		return;
		//there is no result yet
	}
	if (_solved_status!=-1) {
		return;
		//we already reported to Mallob
	}
	if (checkCrossCommNeedsAdvancing("tryReportToMallob")) {
		return;
	}

	int res = _staged_solved_status;
	assert(res!=-1);
	_internal_result.result = res;
	_solved_status = res;
	//this will now be noticed by Mallob, and will end this App
	LOGGER(_sweeplogger,V2_INFO, "SWEEP: mainthread now reporting to Mallob: result %i \n", _solved_status);
	LOG(                V2_INFO, "SWEEP: mainthread now reporting to Mallob: result %i \n", _solved_status);
}



bool SweepJob::checkCrossCommNeedsAdvancing(const std::string &from) {
	if (!_clause_comm) {
		return false;
	}
	// Before reporting a result via the below line, checks via
	//_clause_comm->hasLocalClausesLeftToShare();
	// (should be from the main thread) if there's still some clauses that need to be shared.
	// In that case, appl_communicate() needs to call this from time to time:
	// _clause_comm->feedLocalClausesIntoCrossSharing(buffer, nullptr);
	// (with an empty buffer from a BufferBuilder) since this will initiate XTCS operations.
	if (_staged_solved_status!=-1 || _terminate_all) {
		//We want to finish, check whether cross-job-communication has some unfinished business
		if (_clause_comm->hasLocalClausesLeftToShare()) {
			LOGGER(_sweeplogger, V3_VERB, "SWEEP _clause_comm->hasLocalClausesLeftToShare() == true . Advancing now. Called from:  %s \n", from.c_str());
			//try to get the cross-job sharer to finish
			for (int i=0; i<5; i++) {
				BufferBuilder bb(-1,10,false);
				auto buffer = bb.extractBuffer();
				_clause_comm->feedLocalClausesIntoCrossSharing(buffer, nullptr);
			}
			_clause_comm->communicate();
			return true;
		}
	}
	return false;
}


void SweepJob::appl_communicate(int sourceRank, int mpiTag, JobMessage& msg) {
	if (_params.crossJobCommunication() && _clause_comm) {
		if (_clause_comm->handle(sourceRank, mpiTag, msg)) {
			LOGGER(_sweeplogger,V5_DEBG, " _clause_comm->handle() got message in SweepJob::communicate \n");
			return;
		}
	}
	if (mpiTag != MSG_SEND_APPLICATION_MESSAGE) {
		LOGGER(_sweeplogger,V1_WARN, "SWEEP MSG WARN [%i] got unexpected message with mpiTag=%i, msg.tag=%i  (instead of MSG_SEND_APPLICATION_MESSAGE mpiTag == 30)\n", _my_rank, mpiTag, msg.tag);
	}
	if (msg.returnedToSender) {
		LOGGER(_sweeplogger,V1_WARN, "SWEEP MSG WARN [%i]: received unexpected returnedToSender message during Sweep Job Workstealing! source=%i mpiTag=%i, msg.tag=%i treeIdxOfSender=%i, treeIdxOfDestination=%i \n", _my_rank, sourceRank, mpiTag, msg.tag, msg.treeIndexOfSender, msg.treeIndexOfDestination);
	}
	else if (msg.tag == TAG_SEARCHING_WORK) {
		assert(msg.payload.size() == NUM_SEARCHING_WORK_FIELDS);
		int sourceLocalId = msg.payload[0];
		int sourceContextId = msg.payload[1];
		int sourceTreeIndex = msg.payload[2];
		msg.payload.clear();
		LOGGER(_sweeplogger,V4_VVER, "SWEEP MSG [%i] <---?---- [%i](%i) \n", _my_rank, sourceRank, sourceLocalId);
		if (_terminate_all.load(std::memory_order_relaxed)) {
			LOGGER(_sweeplogger,V4_VVER, "SWEEP Not answering to post-termination MPI steal request from [%i](%i) \n", sourceRank, sourceLocalId);
			return;
		}
		auto locally_stolen_work = stealWorkFromAnyLocalSolver(sourceRank, sourceLocalId);
		int stolen_count = locally_stolen_work.size();
		msg.payload = std::move(locally_stolen_work);
		msg.payload.push_back(sourceLocalId);
		//send back to source
		msg.tag = TAG_RETURNING_STEAL_REQUEST;
		int sourceIndex = getJobComm().getInternalRankOrMinusOne(sourceRank);
		msg.treeIndexOfDestination = sourceIndex;
		msg.contextIdOfDestination = getJobComm().getContextIdOrZero(sourceIndex);
		//in case we don't have full tree information about the origin of the message we can still send it back,
		//because we can use metadata in the message itself as a backup
		if (msg.contextIdOfDestination != 0) {
			assert(msg.contextIdOfDestination == sourceContextId);
		} else {
			msg.contextIdOfDestination = sourceContextId;
			LOGGER(_sweeplogger,V1_WARN,"WARN SWEEP: SEARCHING_WORK receiver [%i] does not know contextIdOfDestination of sender [%i], now using ContextId %i provided by incoming msg itself\n", _my_rank, sourceRank, sourceContextId);
		}
		if (msg.treeIndexOfDestination != -1) {
			assert(msg.treeIndexOfDestination == sourceTreeIndex);
		} else {
			msg.treeIndexOfDestination = sourceTreeIndex;
			LOGGER(_sweeplogger,V1_WARN,"WARN SWEEP: SEARCHING_WORK receiver [%i] does not know treeIndexOfDestination of sender [%i], now using treeIndex %i provided by incoming msg itself\n", _my_rank, sourceRank, sourceTreeIndex);
		}
		//probably happens when sweep message slips right into the ongoing ranklist update
		//that is periodically started as aggregating, in job.cpp 233
		assert(msg.contextIdOfDestination != 0 ||
			log_return_false("SWEEP STEAL ERROR: invalid contextIdOfDestination==0. In TAG_RETURNING_STEAL_REQUEST, wanted to return an message"
					"With sourceRank=%i, sourceIndex=%i, payload.size()=%zu \n", sourceRank, sourceIndex, msg.payload.size()));

		assert(msg.treeIndexOfDestination >= 0 ||
			log_return_false("SWEEP STEAL ERROR: treeIndexOfDestination < 0 . In TAG_RETURNING_STEAL_REQUEST, wanted to return an message"
					"With sourceRank=%i, sourceIndex=%i, contextIdOfDestination=%i, payload.size()=%zu \n", sourceRank, sourceIndex, msg.contextIdOfDestination, msg.payload.size()));

		if (stolen_count>0) {
			LOGGER(_sweeplogger,V4_VVER, "SWEEP snd [%i] >>>>%i>>>> [%i](%i) \n", _my_rank, stolen_count, sourceRank, sourceLocalId);
		}
		getJobTree().send(sourceRank, MSG_SEND_APPLICATION_MESSAGE, msg);
	}
	else if (msg.tag == TAG_RETURNING_STEAL_REQUEST) {
		int stealingLocalId = msg.payload.back(); msg.payload.pop_back();
		auto &request = _worksteal_requests[stealingLocalId];
		assert(request.got_steal_response == false || log_return_false("SWEEP ERROR : got MPI steal answer, but already request.got_steal_response==true.  sourceRank %i, stealingLocalId %i, payload.size %zu ", sourceRank, stealingLocalId, msg.payload.size()));
		assert(request.to_send == false			|| log_return_false("SWEEP ERROR : got MPI steal answer, but still   request.to_send==true.             sourceRank %i, stealingLocalId %i, payload.size %zu ", sourceRank, stealingLocalId, msg.payload.size()));
		request.t_received = Timer::elapsedSeconds();
		request.stolen_work = std::move(msg.payload);
		request.got_steal_response = true;
		if (request.stolen_work.size() > 0)
			LOGGER(_sweeplogger,V4_VVER, "SWEEP rcv [%i](%i) <<<%zu<<<< [%i]\n", _my_rank, stealingLocalId, request.stolen_work.size(), sourceRank );
		else
			LOGGER(_sweeplogger,V4_VVER, "SWEEP rcv [%i](%i) <---0---- [%i]\n", _my_rank, stealingLocalId, sourceRank );
	}
	else if (msg.tag == TAG_FOUND_UNSAT) {
		LOGGER(_sweeplogger,V2_INFO, "SWEEP MSG [%i] <~~~ Found UNSAT! [%i]\n", _my_rank, sourceRank );
		assert(_is_root);
		rootReportSolverResult(nullptr, UNSAT);
	}
	else if (mpiTag == MSG_NOTIFY_JOB_ABORTING)    {LOGGER(_sweeplogger,V1_WARN, "SWEEP MSG WARN [%i]: received NOTIFY_JOB_ABORTING \n", _my_rank);}
	else if (mpiTag == MSG_NOTIFY_JOB_TERMINATING) {LOGGER(_sweeplogger,V1_WARN, "SWEEP MSG WARN [%i]: received NOTIFY_JOB_TERMINATING \n", _my_rank);}
	else if (mpiTag == MSG_INTERRUPT)			   {LOGGER(_sweeplogger,V1_WARN, "SWEEP MSG WARN [%i]: received MSG_INTERRUPT \n", _my_rank);}
	else {LOGGER(_sweeplogger,V1_WARN, "SWEEP MSG WARN [%i]: received unexpected mpiTag %i with msg.tag %i \n", _my_rank, mpiTag, msg.tag);}
}

void SweepJob::appl_terminate() {
	LOGGER(_sweeplogger,V2_INFO, "SWEEP [%i] (job #%i) got TERMINATE signal via appl_terminate() \n", _my_rank,getId());
	LOG(                V2_INFO, "SWEEP [%i] (job #%i) got TERMINATE signal via appl_terminate() \n", _my_rank,getId());
	if (!_terminate_all) {
		_terminate_all = true;
		triggerTerminations();
	} else {
		LOGGER(_sweeplogger,V2_INFO, "SWEEP [%i] (job #%i) already triggered terminations on its own, skipping this second external trigger to avoid concurrent accesses\n", _my_rank,getId());
	}
}


void SweepJob::appl_memoryPanic() {
	LOGGER(_sweeplogger,V1_WARN, "[WARN] SWEEP [%i]: Memory panic! \n",_my_rank);
	LOG(                V1_WARN, "[WARN] SWEEP [%i]: Memory panic! \n",_my_rank);
}

bool SweepJob::appl_isDestructible() {
	if (_clause_comm && !_clause_comm->isDestructible()) {
		for (int i = 0; i < 10; i++) _clause_comm->communicate(); // may advance destructibility
		LOGGER(_sweeplogger,V3_VERB, "SWEEP TERM #%i ctx %i [%i] isDestructible? no. _clause_comm not destructible yet\n",  getId(),_my_ctx_id,  _my_rank);
		return false;
	}
	int _running_sweepers = _started_sweepers_count - _finished_sweepers_count;
	if (_finished_sweepers_count < _nThreads) {
		LOGGER(_sweeplogger,V3_VERB, "SWEEP TERM #%i ctx %i [%i] isDestructible? no. only %i/%i finished, %i running \n",  getId(),_my_ctx_id, _my_rank, _finished_sweepers_count.load(), _nThreads, _running_sweepers);
		return false;
	}
	//all background workers are completely done,
	//so joining them now should happen immediately (even doing it sequentially)
	LOGGER(_sweeplogger,V2_INFO, "SWEEP TERM #%i ctx %i [%i] isDestructible? yes. now joining... \n",  getId(), _my_ctx_id,  _my_rank);
	int i=0;
	for (auto &bg_worker : _bg_workers) {
		if (bg_worker->isRunning()) {
			LOGGER(_sweeplogger,V3_VERB, "SWEEP TERM #%i [%i] joining bg_worker    (%i) \n",  getId(),_my_rank, i);
			bg_worker->stop();
			LOGGER(_sweeplogger,V3_VERB, "SWEEP TERM #%i [%i] joined  bg_worker (%i) \n",  getId(),_my_rank, i);
		}
		i++;
	}
	LOGGER(_sweeplogger,V2_INFO, "SWEEP TERM #%i ctx %i [%i] isDestructible? yes. all joined \n",  getId(),_my_ctx_id, _my_rank);
	return true;
}


void SweepJob::checkForUnsatResults() {
	//If we have an UNSAT result, send it to the root node
	//Makes life easier when only the root node reports to Mallob and to the log files
	//for additional robustness we keep sending this message repeatedly
	//(Can also be a self-message), even though technically only sending it once should be enough
	if (_do_report_UNSAT_to_root) {
		auto msg = getMessageTemplate();
		msg.tag = TAG_FOUND_UNSAT;
		LOGGER(_sweeplogger,V2_INFO, "SWEEP [%i] (job #%i) sending UNSAT to root \n", _my_rank,getId());
		getJobTree().sendToRoot(msg);
	}

}


void SweepJob::rootReportSolverResult(KissatPtr sweeper, int res) {
	assert(_is_root || log_return_false("SWEEP ERROR: non-root tries to report result to mallob\n"));
	if (res==UNSAT) {
		//an UNSAT result doesnt come with a proof and can arrive via MPI from another process,
		//so we don't have access to that particular reporting sweeper, nor do we need it, -->nullptr
		assert(sweeper==nullptr);
	}
	//CAREFUL: sweeper is now nullptr in case of UNSAT
	//report exactly once to Mallob, ignore all additional internal reports
	//(can happen if multiple UNSAT messages arrive from other ranks/solvers)
	int expected = -1;
	if (_root_reported_result.compare_exchange_strong(expected, res)) {
		//we are first, continue reporting
	} else {
		if (res != _root_reported_result.load()) {
			LOGGER(_sweeplogger,V3_VERB, "SWEEP WARN [%i]: wanted to report result %i, but was stopped because there is already a different reported result %i \n", _my_rank, res, _root_reported_result.load());
		}
		return;
	}

	//CAREFUL: sweeper is now nullptr in case of UNSAT
	LOGGER(_sweeplogger,V2_INFO, "SWEEP JOB [%i] stages sweep result %i to Mallob\n", _my_rank, res);
	//something would be off if we called this function more than once
	assert(_staged_solved_status == -1 || log_return_false("SWEEP ERROR: duplicate attempt to report result to mallob, was already reported as %i \n", _internal_result.result));
	std::vector<int> formula = {};
	if (res==UNSAT) {
		formula = {};
	} else if (res==IMPROVED){
		assert(sweeper);
		formula = sweeper->extractPreprocessedFormula();
		LOGGER(_sweeplogger,V2_INFO, "SWEEP JOB [%i]: Solution IMPROVED, payload size %zu\n", _my_rank, formula.size());
	} else if (res==UNKNOWN) {
		assert(sweeper);
		// No progress has been made.
		// Design choice: we don't send any formula back, since there would be no new information in it
		// formula = sweeper->extractPreprocessedFormula();
		formula = {};
		LOGGER(_sweeplogger,V2_INFO, "SWEEP JOB [%i]: Solution UNKNOWN, payload size %zu\n", _my_rank, formula.size());
	} else {
		LOGGER(_sweeplogger,V1_WARN, "WARN SWEEP [%i]: unexpected result code %i when reporting to mallob \n", _my_rank, res);
	}
	LOG(                V2_INFO, "SWEEP_RESULT_CODE %i == %s \n", res, res==40 ? "IMPROVED" : res==20 ? "UNSATISFIABLE" : "UNKNOWN");
	LOGGER(_sweeplogger,V2_INFO, "SWEEP_RESULT_CODE %i == %s \n", res, res==40 ? "IMPROVED" : res==20 ? "UNSATISFIABLE" : "UNKNOWN");
	_internal_result.setSolutionToSerialize(formula.data(), formula.size());
	_staged_solved_status = res;
}


void SweepJob::cbReportIteration(int localId) {
	assert(_is_root || log_return_false("SWEEP ERROR : iteration report in a non-root rank. Technically possible, but currently not allowed \n"));
	assert(localId == _representative_localId);
	KissatPtr sweeper = _sweepers[localId];
	assert(sweeper);
	auto stats = sweeper->fetchSweepStats();
	LOGGER(_sweeplogger, V2_INFO, "\n");
	LOGGER(_sweeplogger,V2_INFO, "Reported by [%i](%i)		\n", _my_rank, localId);
	LOGGER(_sweeplogger,V2_INFO, "ITERATION_CURR   %i		    \n", stats.local_iteration);
	LOGGER(_sweeplogger,V2_INFO, "ITERATIONS_MAX     %i		\n", _params.sweepMaxIterations());
	LOGGER(_sweeplogger,V2_INFO, "TIME_APP_TOTAL          %.3f s\n",  Timer::elapsedSeconds() - _timestamp_start_sweepapp);
	LOGGER(_sweeplogger,V2_INFO, "TIME_BEFORE_SOLVING     %.3f s\n",  _timestamp_started_synchronized_solving - _timestamp_start_sweepapp);
	LOGGER(_sweeplogger,V2_INFO, "ACTIVE_PRCNT            %.2f %\n", 100*(double)stats.curr_active/(double)stats.orig_vars);
	LOGGER(_sweeplogger,V2_INFO, "ENV_LIMIT_VARS    %i 		\n", stats.env_limit_vars);
	LOGGER(_sweeplogger,V2_INFO, "ENV_LIMIT_DEPTH   %i 		\n", stats.env_limit_depth);
	LOGGER(_sweeplogger,V2_INFO, "ENV_LIMIT_CLAUSES %i 		\n", stats.env_limit_clauses);
	LOGGER(_sweeplogger,V2_INFO, "ROUNDS_THISITER     %i   	\n",_root_rounds_this_iteration);

	LOGGER(_sweeplogger,V2_INFO, "CLAUSES_CURR        %i 		\n", stats.clauses);
	LOGGER(_sweeplogger,V2_INFO, "CLAUSES_START       %i 		\n", stats.start_clauses);
	LOGGER(_sweeplogger,V2_INFO, "BINIRR_CURR             %i	\n", stats.binirr);
	LOGGER(_sweeplogger,V2_INFO, "BINIRR_START            %i	\n", stats.start_binirr);
	LOGGER(_sweeplogger,V2_INFO, "ACTIVE_CURR      %i			\n", stats.curr_active);
	LOGGER(_sweeplogger,V2_INFO, "ACTIVE_START     %i			\n", stats.start_active);
	LOGGER(_sweeplogger,V2_INFO, "FIXED_CURR           %i	\n", stats.orig_vars - stats.curr_active);
	LOGGER(_sweeplogger,V2_INFO, "FIXED_START          %i	\n", stats.orig_vars - stats.start_active);
	LOGGER(_sweeplogger,V2_INFO, "FIXED_IN_CEC             %i	\n", stats.start_active - stats.curr_active);
	LOGGER(_sweeplogger,V2_INFO, "ELIMINATED       %i 		\n", stats.curr_eliminated);
	LOGGER(_sweeplogger,V2_INFO, "NEWUNITS         %i 		\n", stats.curr_units - stats.start_units);
	LOGGER(_sweeplogger,V2_INFO, "ALLUNITS         %i 		\n", stats.curr_units);
	LOGGER(_sweeplogger,V2_INFO, "SUM_ELIM_NEWU            %i	\n", stats.curr_eliminated + stats.curr_units - stats.start_units );
	LOGGER(_sweeplogger,V2_INFO, "SWEEPUNITS       %i 		\n", stats.sweep_units);
	LOGGER(_sweeplogger,V2_INFO, "EQUIVALENCES     %i 		\n", stats.sweep_eqs);
	LOGGER(_sweeplogger,V2_INFO, "SUM_SWEEP_EU             %i  \n", stats.sweep_eqs + stats.sweep_units);
	LOGGER(_sweeplogger,V2_INFO, "\n");
}


void SweepJob::reportStealLatencies(KissatPtr sweeper) {
	//Write detailed information about local and MPI workstealing events
	if (LOGGER_STATIC_VERBOSITY >= 5) {
		//super verbose, print every individual steal
		Logger _steallogger(Logger::getMainInstance().copy("<STEAL>", ".steal."+to_string(sweeper->getLocalId())));
		for (auto steal : sweeper->steal_records) {
			string stealtype = steal.stealtype == SweepStealType::MPI ? "mpi" : "local";
			LOGGER(_steallogger, V5_DEBG, "id %i   r %i   nr %i   ts %.5f  trc %.5f  trd %.5f   d %.6f   s %i  %s\n",sweeper->getLocalId(), steal.round, steal.nr,  steal.t_submit, steal.t_receive, steal.t_read, steal.t_receive - steal.t_submit, steal.size,  stealtype.c_str() );
		}
	}
	//Still very verbose: Print steal information aggregated for each sharing round
	//The last sweeper to contribute also reports the aggregated statistics
	{
		std::lock_guard<std::mutex> lock(_stealinfo_mutex);
		_stealinfos_per_solver.emplace_back(sweeper->steal_records);
		if (_stealinfos_per_solver.size() == _nThreads) {
			//Organize steal info for each round
			auto _stealinfos_per_round = std::vector<std::vector<SweepStealInfo>>(_lastImportedRound+1);
			for (auto &records_of_solver : _stealinfos_per_solver) {
				for (auto &info : records_of_solver) {
					assert(info.round < _stealinfos_per_round.size() || log_return_false("SWEEP ERROR : trying to sort stealinfo of round %i into too small provisioned vector of size %i \n", info.round, _stealinfos_per_round.size()));
					_stealinfos_per_round[info.round].push_back(info);
				}
			}
			Logger _stealsumlogger(Logger::getMainInstance().copy("<STEALSUM>", ".stealsum"));
			int round = 0;
			for (auto &roundlist : _stealinfos_per_round) {
				round++;
				int iteration = _iteration_of_round[round];
				int loc_stolensum = 0;
				int mpi_stolensum = 0;
				std::vector<float> local{};
				std::vector<float> mpi{};
				for (auto &info : roundlist) {
					float latency = info.t_receive - info.t_submit;
					if (info.stealtype==SweepStealType::Local) {
						loc_stolensum += info.size;
						local.push_back(latency);
					} else {
						mpi_stolensum += info.size;
						mpi.push_back(latency);
					}
				}
				if (!local.empty() || !mpi.empty() || loc_stolensum || mpi_stolensum) {
					LOGGER(_stealsumlogger, V4_VVER, "iter %i rnd %i   locA %i   mpiA %i   locS %i   mpiS %i   max_mpi_ms %.3f\n",
						iteration, round, local.size(), mpi.size(), loc_stolensum, mpi_stolensum,
						// local.empty() ? 0.0f : std::accumulate(local.begin(), local.end(), 0.0f) / local.size(),
						// local.empty() ? 0.0f : *std::max_element(local.begin(), local.end()) * 1000,
						// mpi.empty()   ? 0.0f : *std::min_element(mpi.begin(), mpi.end()) * 1000,
						// mpi.empty()   ? 0.0f : 1000 * std::accumulate(mpi.begin(), mpi.end(), 0.0f) / mpi.size(),
						mpi.empty()   ? 0.0f : *std::max_element(mpi.begin(), mpi.end()) * 1000);

				}
			}
		}
	}
}

void SweepJob::reportEndStats(KissatPtr sweeper) {
	assert(_is_root);
	assert(sweeper->getLocalId() == _representative_localId);
	LOGGER(_sweeplogger,V2_INFO, "SWEEP_PRIORITY       %.3f\n", _params.preprocessSweepPriority.val);
	LOGGER(_sweeplogger,V2_INFO, "SWEEP_PROCESSES      %i\n",  getVolume());
	LOGGER(_sweeplogger,V2_INFO, "SWEEP_THREADS_PER_P  %i\n", _nThreads);
	LOGGER(_sweeplogger,V2_INFO, "SWEEP_SHARING_PERIOD_PARAM   %.3f s \n", _params.sweepSharingPeriod.val);
	static const int DURATION_WARN_FACTOR=2;
	if (_timestamp_root_started_bcast.size()>1) {
		float total_sharing_time = _timestamp_root_started_bcast.back() - _timestamp_root_started_bcast.front();
		float avg_sharing_period = total_sharing_time / (_timestamp_root_started_bcast.size()-1);
		LOGGER(_sweeplogger,V2_INFO, "SWEEP_SHARING_PERIOD_AVG     %.3f s \n", avg_sharing_period);
		for (int i=0; i < _timestamp_root_started_bcast.size()-1; i++) {
			float period = _timestamp_root_started_bcast[i+1] - _timestamp_root_started_bcast[i];
			if (period > DURATION_WARN_FACTOR*avg_sharing_period) {
				LOGGER(_sweeplogger,V1_WARN, "[WARN] SWEEP_SHARING_PERIOD_REAL %.3f sec   (round %i) is much larger than average %.4f sec \n", period, i, avg_sharing_period);
			}
		}
	}
	// float max_appl_comm_duration = *std::max_element(_duration_appl_communicate.begin(), _duration_appl_communicate.end());
	LOGGER(_sweeplogger,V2_INFO, "SWEEP_APPL_COMMUNICATE_MAX   %.6f s \n", _max_appl_comm_duration);
	for (int i=0; i<15 && i<_internal_result.getSolutionSize(); i++) {
		LOGGER(_sweeplogger,V3_VERB, "RESULT Sweep Formula[%i] = %i \n", i, _internal_result.getSolution(i));
	}
}

void SweepJob::checkIdleWorkStatus() {
	if (_terminate_all.load(std::memory_order_relaxed)) {
		return;
		//prevents segfault! when termination is triggered, the sweeper references might suddenly become invalid.
		//no touching them anymore
	}

	const float STATUS_PERIOD = 0.0100; //in seconds. Defines the long-term-idle window
	if (Timer::elapsedSeconds() - _timestamp_log_last_idleinfo < STATUS_PERIOD) {
		return;
	}
	_timestamp_log_last_idleinfo = Timer::elapsedSeconds();
	int idles = 0;
	int longterm_idles = 0;
	int open = 0;
	std::ostringstream oss_idles;
	std::ostringstream oss_work;
	for (auto &sweeper : _sweepers) {
		if (sweeper) {
			open++;
			if (sweeper->sweeper_longterm_idle) {
				longterm_idles++;
				oss_idles  << sweeper->getLocalId() << ",";
			}
			if (sweeper->sweeper_is_idle) {
				idles++;
				//those solvers which are idle right now are candidates for being also long-term idle
				sweeper->sweeper_longterm_idle = true;
			}
			oss_work << shweep_get_work_estimate(sweeper->solver) << ",";
		} else {
			oss_work << "--,";
		}
	}
	_lastLongtermIdleCount = longterm_idles;
	LOGGER(_sweeplogger,V4_VVER, "SWEEP [%i] idle(long) %i(%i) %s  Work[%i]: %s\n", _my_rank, idles, longterm_idles, oss_idles.str().c_str(), _my_rank, oss_work.str().c_str());
	countLaggingSolvers();
}

void SweepJob::checkSharingDelay() {
	if (_terminate_all.load(std::memory_order_relaxed))
		return;

	float time = Timer::elapsedSeconds();
	constexpr float MAX_DELAY_FACTOR = 6; //factor over normal sharing round period
	float expected_period = _params.sweepSharingPeriod.val;
	float warn_threshhold = expected_period * MAX_DELAY_FACTOR;
	//Be more lenient if we are inbetween iterations
	//Because for very large instances it can take even a few seconds on the root node to finish up on one iteration (mainly the substitute() call.
	//This end-of-iteration delay is expected and should not count as a problem in the sharing procedure
	constexpr float MAX_DELAY_BETWEEN_ITERATIONS = 4; //seconds
	if (_rank_is_inbetween_iterations) {
		warn_threshhold = MAX_DELAY_BETWEEN_ITERATIONS;
	}
	if (!_timestamp_contributed_to_sharing.empty()) {
		float delay = time - _timestamp_contributed_to_sharing.back();
		if (delay > warn_threshhold) {
			LOGGER(_sweeplogger, V3_VERB, "WARN SWEEP SHARINGDELAY [%i]: %.2f sec since contrib, factor %.1f \n", _my_rank, delay, delay/expected_period);
		}
	}
	if (!_timestamp_receive_sharing_result.empty()) {
		float delay = time - _timestamp_receive_sharing_result.back();
		if (delay > warn_threshhold) {
			LOGGER(_sweeplogger, V3_VERB, "WARN SWEEP SHARINGDELAY [%i]: %.2f sec since recv, factor %.1f \n", _my_rank, delay, delay/expected_period);
		}
	}
}


bool SweepJob::skip_MPI_forNow() {
	if (getVolume()==0) {
		LOGGER(_sweeplogger,V3_VERB, "SWEEP [%i] WARN : getVolume()==0\n", _my_rank);
		return true;
	}
	if (getVolume()==1) {
		return true;
	}
	return (getJobComm().size() < getVolume());
}

void SweepJob::sendWorkstealsViaMPI() {
	if (_terminate_all.load(std::memory_order_relaxed))
		return;

	//Worksteal requests need to be sent by the MPI *main* thread.
	//If kissat-threads themselves send MPI messages, things can crash, since they clash somehow with the MPI hierarchy
	//So each solver-thread queues a steal-request to shared memory,
	//where the main MPI thread can pick it up (here) and sends an MPI msg on behalf of the solver
	for (auto &request : _worksteal_requests) {
		if (request.to_send) {
			int senderLocalId = request.senderLocalId;
			//if we are still in the phase where MPI sends are not done,
			//we short-fuse the requests to a zero dummy and return them to the solver threads
			//same if the whole job is terminated
			if (skip_MPI_forNow()) {
				request.to_send = false;
				request.got_steal_response = true;
				LOGGER(_sweeplogger,V3_VERB, "SWEEP [%i] mainthread SKIP MPI-steal request by (%i) \n", _my_rank, senderLocalId);
				continue;
			}
			//There was no local work available, now we prepare to send out an MPI message
			int my_comm_rank = getJobComm().getWorldRankOrMinusOne(_my_index);
			if (my_comm_rank == -1) {
				LOGGER(_sweeplogger,V3_VERB, "SWEEP SKIP own rank [%i] (myindex %i) <ctx %i> not yet in JobComm of size %zu \n", _my_rank, _my_index, _my_ctx_id, getJobComm().size());
				continue;
			}
			assert(getVolume()>=1 || log_return_false("SWEEP ERROR [%i](%i) in workstealing: getVolume()==%i, i.e. no volume available to steal from\n", _my_rank, senderLocalId, getVolume()));
			assert(request.targetIndex==-1);
			assert(request.targetRank==-1);
			//Shuffle/Permute list of all solvers we could steal from to
			//1. have a random stealing order, and
			//2. still know that we will eventually check all of them, if the first few don't have anything to offer
			std::vector<int> tree_indices = std::vector<int>(getVolume());
			for (int i=0; i<getVolume(); i++) {
				tree_indices[i] = i;
			}
			//created/seeded only once per main mallob thread, then just advancing rng calls
			static thread_local std::mt19937 rng(std::random_device{}());
			std::shuffle(tree_indices.begin(), tree_indices.end(), rng);

			//verbose dummy variables to make sure we really only touch the request once we have a valid index+rank combo
			int foundIndex = -1;
			int foundRank = -1;
			for (int targetIndex : tree_indices) {
				if (targetIndex==_my_index) {
					// not stealing from ourselves, roll again, and don't count this roll
					continue;
				}
				int targetRank = getJobComm().getWorldRankOrMinusOne(targetIndex);
				if (targetRank == -1) {
					//target rank of this targetIndex is not yet in JobTree, might need some more milliseconds to update, roll again
					LOGGER(_sweeplogger,V3_VERB, "SWEEP SKIP target idx %i not in JobComm (size %zu) \n", targetIndex, getJobComm().size());
					continue;
				}
				if (getJobComm().getContextIdOrZero(targetIndex)==0) {
					//target is not yet listed in address list. Might happen for a short period just after it is spawned. roll again
					LOGGER(_sweeplogger,V3_VERB, "SWEEP SKIP ctx_id of target is missing. getVolume()=%i, rndTargetIndex=%i, rndTargetRank=%i, myIndex=%i, myRank=%i, JobComm size %zu \n", getVolume(), targetIndex, targetRank, _my_index, _my_rank, getJobComm().size());
					continue;
				}
				foundRank = targetRank;
				foundIndex = targetIndex;
				break;
			}

			if (foundIndex==-1 || foundRank==-1) {
				//couldn't find a target for this request, skip it for now and process the next
				LOGGER(_sweeplogger,V4_VVER, "SWEEP MSG [%i](%i) SKIP ------ no target possible yet  \n", _my_rank, request.senderLocalId);
				continue;
			}
			request.targetIndex = foundIndex;
			request.targetRank = foundRank;
			assert(request.targetIndex>=0 || log_return_false("SWEEP ERROR: request.targetIndex %i \n", request.targetIndex));
			assert(request.targetRank>=0  || log_return_false("SWEEP ERROR: request.targetRank %i \n", request.targetIndex));
			JobMessage msg = getMessageTemplate();
			msg.tag = TAG_SEARCHING_WORK;
			//Need to add these two fields because we are doing arbitrary point-to-point communication
			msg.treeIndexOfDestination = request.targetIndex;
			msg.contextIdOfDestination = getJobComm().getContextIdOrZero(request.targetIndex);
			assert(msg.contextIdOfDestination != 0 || log_return_false("SWEEP ERROR: contextIdOfDestination==0 in workstealing request! Source rank=%i, targetRank %i \n", _my_rank, request.targetRank));
			assert(msg.treeIndexOfDestination >= 0 || log_return_false("SWEEP ERROR: treeIndexOfDestination < 0 in workstealing request! Source rank=%i, targetRank %i \n", _my_rank, request.targetRank));
			//We also send our contextId and treeIndex. (myContextId, _my_index)
			//Because it can happen that the receiving rank does not know yet our contextId,
			//This (probably) happens when a worksteal request is sent right in the moment when also ranklist update
			//is propagating through all ranks, where some ranks (like this here) have already updated, while others have not
			//So as a backup for the receiving rank, we also provide our own contextId for the return message
			int myContextId = getJobComm().getContextIdOrZero(_my_index);
			msg.payload = {request.senderLocalId, myContextId, _my_index};
			assert(msg.payload.size() == NUM_SEARCHING_WORK_FIELDS);
			LOGGER(_sweeplogger,V4_VVER, "SWEEP MSG [%i](%i) ---?---> [%i] \n", _my_rank, request.senderLocalId, request.targetRank);
			getJobTree().send(request.targetRank, MSG_SEND_APPLICATION_MESSAGE, msg);
			request.to_send = false;
		}
	}
}


//For development purposes: A simple interface to communicate some integers between solver and Mallob
//without the need to declare dedicated new functions each time
int SweepJob::cbCustomQuery(int localId, int query) {
	if (query==QUERY_SWEEP_ITERATION) {
		return _root_iteration;
	}
	return 0;
}

void SweepJob::cbImportEq(int *elit1, int *elit2, int localId) {
	assert(*elit1==0);
	assert(*elit2==0);
	KissatPtr sweeper = _sweepers[localId];
	const int idx   = sweeper->curr_eq_index;
	const int round = sweeper->curr_eq_round;
	std::vector<int> &eqs = _imported_data[round].eqs;
	//Try to (continue) importing from the round we are currently reading from
	if (idx < eqs.size()) {
		*elit1 = eqs[idx];
		*elit2 = eqs[idx+1];
		sweeper->curr_eq_index+=2;
		assert(*elit1 !=0 || *elit2 !=0		|| log_return_false("SWEEP ERROR: in cbImportEq: sending invalid empty *elit1=%i, *elit2=0 to the solvers\n", *elit1, *elit2));
		assert(std::abs(*elit1) < std::abs(*elit2)				|| log_return_false("SWEEP ERROR: in cbImportEq: abs(*elit1) is larger than abs(*elit2), but they should be sorted elit1=%i, elit2=%i (index %i, %i,)\n", *elit1, *elit2, idx, idx+1));
		if (sweeper->curr_eq_index == eqs.size()) {
			if (_terminate_all) {
				LOGGER(_sweeplogger,V4_VVER, "SWEEP [%i](%i) ((( < %i > E %i \n", _my_rank, sweeper->getLocalId(),  round, eqs.size()/2);
			} else {
				LOGGER(_sweeplogger,V5_DEBG, "SWEEP [%i](%i) ((( < %i > E %i \n", _my_rank, sweeper->getLocalId(),  round, eqs.size()/2);
			}
		}
	}
	//Check if there is a next round to import
	else if (round < _lastImportedRound.load(memory_order_relaxed)) {
		if (eqs.size()==0) { //for completeness we also log the edge-case where there was nothing to import
			LOGGER(_sweeplogger,V5_DEBG, "SWEEP [%i](%i) ((( < %i > E %i \n", _my_rank, sweeper->getLocalId(), round,  eqs.size()/2);
		}
		//Advance to import from the next stored round data. This does not necessarily need to be the latest round,
		//if there are be multiple rounds stored we might need to catch up sequentially through all of them
		//Keep track how many threads have finished reading this round, such that it can be deleted after all threads have read it
		_finishedRoundCounters[round].threads_finished_eqs++;
		sweeper->curr_eq_round++;
		sweeper->curr_eq_index=0;
		assert(*elit1==0);
		assert(*elit2==0);
		//signal to kissat that there is more to import, it should call again
	} else {
		*elit1 = INVALID_ELIT;
		*elit2 = INVALID_ELIT;
		//signal to kissat that there is nothing more to import
	}
	//now returning to the kissat solver
	//if we didn't have any new equivalcen to provide to the solver,
	//then we didn't touch elit1 & elit2, and the kissat solver notices that we didnt touch them
}


void SweepJob::cbImportUnit(int *elit, int localId) {
	assert(*elit==0);
	KissatPtr sweeper = _sweepers[localId];
	//For comments see cbImportEq (the analog method for importing equalities)
	const int idx   = sweeper->curr_unit_index;
	const int round = sweeper->curr_unit_round;
	std::vector<int> &units = _imported_data[round].units;
	if (idx < units.size()) {
		*elit = units[idx];
		sweeper->curr_unit_index++;
		assert(*elit!=0);
		if (sweeper->curr_unit_index == units.size()) {
			LOGGER(_sweeplogger,V5_DEBG, "SWEEP [%i](%i) ((( < %i > U %i \n", _my_rank, sweeper->getLocalId(), round, units.size() );
		}
	}
	else if (round < _lastImportedRound.load(memory_order_relaxed)) {
		if (units.size()==0) {LOGGER(_sweeplogger,V5_DEBG, "SWEEP [%i](%i) ((( < %i > U %i \n", _my_rank, sweeper->getLocalId(), round, units.size());}
		_finishedRoundCounters[round].threads_finished_units++;
		sweeper->curr_unit_round++;
		sweeper->curr_unit_index=0;
		//leaving elit untouched at zero, signalling to kissat that there is still more to import
	}
	else {
		*elit = INVALID_ELIT;
		//signalling kissat that there is nothing more to import
	}
	//now returning to kissat solver
}

bool SweepJob::tryProvideInitialWork(KissatPtr sweeper) {
	int solver_iteration = shweep_get_curr_iteration(sweeper->solver);
		//we avoid any concurrent business by only providing the initial work to one dedicated solver at the root node
		//by limiting this work provision to the root node, we also have direct access to the flags set by the root transformation of each sharing round
		//we also check explicitly that the solver expects this work for a new iteration, and that we have not already provided it
	if (_is_root
		&& sweeper->getLocalId()==_representative_localId
		&& solver_iteration > _root_iteration
		&& !_root_initwork_startedproviding)
	{
		//already set non-idle here to prevent case where solver is already initialized, non-idle,
		//but still has no work cause its just being copied,
		//and then a sharing operation starts right now, terminating everything wrongly early
		sweeper->sweeper_is_idle = false;
		sweeper->sweeper_longterm_idle = false;

		if (solver_iteration != _root_iteration +1 ) {
			LOGGER(_sweeplogger,V1_WARN, "SWEEP WARN : When providing new work, the solver iteration %i is more than one larger than root_iteration %i \n", solver_iteration, _root_iteration);
		}
		//We need to know how much space to allocate to store each variable "idx" at the array position work[idx],
		//i.e. we need to know max(idx).
		//We assume that the maximum variable index corresponds to the total number of variables
		//i.e. we assume that there are no holes in kissats internal numbering.
		//This is an assumption that standard Kissat makes all the time, so we also do it here

		//this value can be different from numVars here in C++ !! Because kissat might have aready propagated some units, etc.
		const unsigned VARS = shweep_get_num_vars(sweeper->solver);
		LOGGER(_sweeplogger,V2_INFO, "SWEEP WORK PROVIDING --------------%u---------------- for new solver iteration %i (root at %i)\n", VARS, solver_iteration, _root_iteration);
		sweeper->work_received_from_steal = std::vector<int>(VARS);
		_root_initwork_startedproviding = true;

		//the initial work is all variables
		for (int idx = 0; idx < VARS; idx++) {
			sweeper->work_received_from_steal[idx] = idx;
		}

		if (_params.sweepShuffleWork()) {
			//rng created/seeded only once per thread, then just advancing rng calls
			static thread_local std::mt19937 rng19937(std::random_device{}());
			std::shuffle(sweeper->work_received_from_steal.begin(), sweeper->work_received_from_steal.end(), rng19937);
			for (int i=0; i <8 && i<sweeper->work_received_from_steal.size(); i++) {
				LOGGER(_sweeplogger,V2_INFO, "SWEEP WORK Shuffle view: %i\n", sweeper->work_received_from_steal[i]);
			}
		}
		LOGGER(_sweeplogger,V2_INFO, "SWEEP WORK PROVIDED  -------------%u----------------> to sweeper [%i](%i)\n", VARS, _my_rank, sweeper->getLocalId());
		_root_initwork_provided = true;
		return true;
	}

	return false;

}

bool SweepJob::canSolverExitStealing(KissatPtr sweeper) {
	int localId = sweeper->getLocalId();
	auto &request = _worksteal_requests[localId];
	if (shweep_get_end_iteration_signal(sweeper->solver)) {
		LOGGER(_sweeplogger,V4_VVER, "Sweeper [%i](%i) exit mallob steal (end_iter)\n", _my_rank, localId);
		return true;
	}

	if (_terminate_all.load(std::memory_order_relaxed)) {
		LOGGER(_sweeplogger,V4_VVER, "Sweeper [%i](%i) exit mallob steal (terminate_all)\n", _my_rank, localId);
		sweeper->count_repeated_missed_termination++;
		if (sweeper->count_repeated_missed_termination % sweeper->WARN_ON_REPEATED_MISSED_TERMINATION==0) {
			LOGGER(_sweeplogger,V3_VERB, "SWEEP WARN : Sweeper [%i](%i) in %i-th worksteal loop after termination\n", _my_rank, localId, sweeper->count_repeated_missed_termination);
		}
		return true;
	}
	return false;
}

void SweepJob::solverGoStealing(KissatPtr sweeper) {
	int localId = sweeper->getLocalId();
	sweeper->work_received_from_steal = {};

	if (tryProvideInitialWork(sweeper)) {
		//we successfully provided the initial work to this solver
		return;
	}

	sweeper->sweeper_is_idle = true;

	//This is the fixed request "slot" we are using to communicate concurently via shared memory with the main worker thread
	auto &request = _worksteal_requests[localId];

	constexpr int MPI_WAIT_TIME_MILLISECS = 10;

	//Check whether a previously queued MPI request has been answered
	if (request.got_steal_response) {
		LOGGER(_sweeplogger,V4_VVER, "Sweeper [%i](%i) got response nr %i \n", _my_rank, localId, request.nr);
		if (_terminate_all) {
			LOGGER(_sweeplogger,V3_VERB, "Sweeper [%i](%i) got steal response nr. %i\n", _my_rank, localId, request.nr);
		}
		float t1 = Timer::elapsedSeconds();
		request.t_read = t1;
		int size = request.stolen_work.size();
		assert(request.to_send  == false			|| log_return_false("SWEEP ERROR : got request response, but still   request.to_send==true.             stealingLocalId %i, payload.size %zu ", localId, size));
		assert(request.is_active		    		|| log_return_false("SWEEP ERROR : got request response, but was no longer flagged active.               stealingLocalId %i, payload.size %zu ", localId, size));
		if (_params.verbosity()>=V4_VVER) {
			sweeper->steal_records.push_back({request.nr, SweepStealType::MPI, size , request.t_queued, request.t_received ,  t1, _lastImportedRound });
		}
		request.got_steal_response = false; //to not read it a second time
		request.is_active = false; //request was fully processed, the slot is now inactive
		if (size>0) {
			sweeper->work_received_from_steal = std::move(request.stolen_work);
			LOGGER(_sweeplogger,V4_VVER, "SWEEP recv [%i](%i) <==%zu==== [%i] \n",  _my_rank, localId, size, request.targetRank);
			// return;
		}
		//update: with the steal-loop now happening in Mallob-Level, we need to return here to allow the steal-loop
		//to break if the iteration ended, and not immediately schedule another request
		return;
	}

	if (request.is_active) {
		//an MPI request is queued, we wait for it's response and will not try to steal locally meanwhile
		//since the request is queued, there is nothing else to do for now, can way ~2ms
		const float STEALDELAY_WARN_TRIGGER=0.1; //Warn if MPI stealing request is not answered after 100ms
		float delay = Timer::elapsedSeconds() - request.t_queued;
		if (delay > STEALDELAY_WARN_TRIGGER) {
			LOGGER(_sweeplogger,V3_VERB, "WARN STEALDELAY at [%i](%i)! waiting %f s  \n", _my_rank, localId, delay);
		}

		LOGGER(_sweeplogger,V4_VVER, "Sweeper [%i](%i) waiting for MPI nr. %i \n", _my_rank, localId, request.nr);
		//can wait for some milliseconds, nothing to do
		usleep(1000 * MPI_WAIT_TIME_MILLISECS);
		// return;
		//update: by bypassing this return, the solver can steal locally, even if there is also an MPI request pending
	}

	//No success via MPI (either there was no answer, or the answer had no work).
	//Next: try local steal on this rank
	float t0 = Timer::elapsedSeconds();
	auto stolen_work = stealWorkFromAnyLocalSolver(_my_rank, localId);
	float t1 = Timer::elapsedSeconds();
	int size = stolen_work.size();
	int nr = sweeper->attempted_steals++;
	if (_params.verbosity()>=V4_VVER) {
		sweeper->steal_records.push_back({nr, SweepStealType::Local, size, t0, t1, t1, _lastImportedRound });
	}
	if (size>0) {
		//Successful local steal
		sweeper->work_received_from_steal = std::move(stolen_work);
		LOGGER(_sweeplogger,V4_VVER, "SWEEP lcl [%i](%i) <==%zu====  \n", _my_rank, localId, size, _my_rank);
		return;
	}

	//No success with local steal. Now we try to steal from some other ranks.
	//To this end, deposit a steal request in a shared memory array,
	//the main MPI thread will then pick it up and send an appropriate MPI message
	//This indirection is necessary because sending MPI messages from any other thread
	//(like this solver thread) can cause MPI problems
	if (request.is_active == false && !skip_MPI_forNow()) {
		request.newQueuedRequest(localId, sweeper->attempted_steals++);
		LOGGER(_sweeplogger,V4_VVER, "Sweeper [%i](%i) queued nr. %i \n", _my_rank, localId, request.nr);
	}

	// If we make it until here we are waiting for work and have have nothing else to do for now.
	// Can wait for some millisecond until we check the system again.
	usleep(1000 * MPI_WAIT_TIME_MILLISECS);
}

void SweepJob::cbStealWorkNew(unsigned **work, int *work_size, int localId) {
	//this array access is safe because the callback is called by this sweeper itself
	KissatPtr sweeper = _sweepers[localId];

	//main loop in which a solver sits while it tries to steal work from others
	while (true) {
		shweep_do_EU_imports(sweeper->solver);
		solverGoStealing(sweeper);
		if (canSolverExitStealing(sweeper)) {
			sweeper->sweeper_is_idle = true;
			sweeper->work_received_from_steal = {};
			break;
		}
		if (sweeper->work_received_from_steal.size()>0) {
			break;
		}
	}
	//We store the steal data persistently in the C++ vector sweeper->work_received_from_steal, allocated and managed in C++
	//The kissat solver (and other solver threads) will then be allowed to read and write(!) within this fixed allocated memory.
	*work = reinterpret_cast<unsigned int*>(sweeper->work_received_from_steal.data());
	*work_size = (int)sweeper->work_received_from_steal.size();
	if (*work_size != sweeper->work_received_from_steal.size()) {
		LOGGER(_sweeplogger,V1_WARN, "SWEEP WARN ERROR : weird work discrepancy: *work_size==%i, work.size()==%zu \n", *work_size, sweeper->work_received_from_steal.size());
	}
	assert(*work_size>=0 || log_return_false("SWEEP ERROR : work size %i \n", *work_size));
	if (*work_size>0) {
		sweeper->sweeper_is_idle = false;
		sweeper->sweeper_longterm_idle = false;
	}
	if (_terminate_all)	LOGGER(_sweeplogger,V4_VVER, "Sweeper [%i](%i) returning from stealing to solver\n", _my_rank, localId);
	//callback ends, solver thread returns back to kissat C solver code
}

void SweepJob::rootStartNewSharingRound() {
	if (!_is_root)
		return;
	if (_terminate_all.load(std::memory_order_relaxed))
		return;

	//only the root node initiates sharing rounds
	assert(_is_root);

	if (!_bcast) {
		LOGGER(_sweeplogger,V1_WARN, "SWEEP WARN : SHARE BCAST root couldn't initiate sharing round, _bcast is Null\n");
		return;
	}

	if (_timestamp_root_started_bcast.size()>0 &&  Timer::elapsedSeconds() < _timestamp_root_started_bcast.back() + _params.sweepSharingPeriod.val) {
		//not yet time for next sharing round
		return;
	}

	if (!_flag_started_synchronized_solving.load(std::memory_order_relaxed)) {
		const float LOG_PERIOD = 0.100;
		float t = Timer::elapsedSeconds();
		if (t > _timestamp_log_notonline + LOG_PERIOD) {
			_timestamp_log_notonline = t;
			LOGGER(_sweeplogger,V3_VERB, "SWEEP root: Delay first round, not all solvers online yet (%i/%i) \n", _started_sweepers_count.load(), _nThreads);
		}
		return;
	}

	if (!_root_initwork_startedproviding) {
		//Print these infos only once ever 100ms
		const float LOG_PERIOD = 0.100;
		float t = Timer::elapsedSeconds();
		if (t > _timestamp_log_delayedround + LOG_PERIOD) {
			_timestamp_log_delayedround = t;
			if (_root_iteration==0) {
				LOGGER(_sweeplogger,V3_VERB, "root: Delay first sharing round, CCC still running\n");
			} else {
				LOGGER(_sweeplogger,V3_VERB, "root: Delay next sharing round, haven't started to provide initial work\n");
			}
		}
		return;
		//It seems to be the sweet-spot to wait with initiating a new sharing round until we *started* to provide initial work to the representative solver
		//If we didn't check for initial work at all, then it can happen that we have multiple sharing rounds after an iteration ends,
		//which can share all_idle multiple times and could lead to skipped iterations
		//If we waited longer, for example until the work was actually provided,
		//then for large instance we would wait here for quite some time and other ranks would start issuing Sharingdelay warnings
	}

	//make sure that only one sharing operation is going on at a time
	//on this root node, hasReceivedBroadcast is equivalent to asking whether this _bcast object has already started a broadcast
	if (_bcast->hasReceivedBroadcast()) {
		LOGGER(_sweeplogger,V4_VVER, "SWEEP root: Delay next round. round %i still ongoing\n", _root_sharing_round);
		return;
	}
	//Broadcast a ping to all workers to initiate an AllReduce
	//The broadcast includes all workers currently reachable by the root-node and informs them about their parent and potential children
	//It then causes the leaf nodes to call the callback, initiating the AllReduce
	// _root_last_sharing_start_timestamp = Timer::elapsedSeconds();
	_timestamp_root_started_bcast.push_back(Timer::elapsedSeconds());
	LOGGER(_sweeplogger,V4_VVER, "SWEEP root: Initiating new sharing round via modular broadcast\n");
	JobMessage msg = getMessageTemplate();
	msg.tag = _bcast->getMessageTag();
	msg.payload = {};
	_bcast->broadcast(std::move(msg));
}

void SweepJob::appendMetadataToReductionElement(std::vector<int> &contrib, const Metadata &md) {
	const size_t offset = contrib.size();
	contrib.resize(offset + NUM_METADATA_FIELDS);
	std::memcpy(contrib.data() + offset, &md, sizeof(md));
}

SweepJob::Metadata SweepJob::readMetadataFromReductionElement(const std::vector<int> &contrib) {
	assert(contrib.size() >= (size_t)NUM_METADATA_FIELDS);
	Metadata md;
	std::memcpy(&md, contrib.data() + contrib.size() - NUM_METADATA_FIELDS, sizeof(md));
	return md;
}

void SweepJob::writeMetadataToReductionElement(std::vector<int> &contrib, const Metadata &md) {
	assert(contrib.size() >= (size_t)NUM_METADATA_FIELDS);
	std::memcpy(contrib.data() + contrib.size() - NUM_METADATA_FIELDS, &md, sizeof(md));
}

void SweepJob::cbContributeToAllReduce() {
	assert(_bcast);
	assert(_bcast->hasResult());
	//bcast hasResult present means that this Process got responses from all its children,
	//so the tree structure is correctly known, and we can continue with a contribution and reduction
	auto snapshot = _bcast->getJobTreeSnapshot();
	LOGGER(_sweeplogger,V4_VVER, "SWEEP [%i] BCAST complete, callback creating RED & contributing (%i)children: (%i)[%i] , (%i)[%i]  \n",
		_my_rank, snapshot.nbChildren, snapshot.leftChildIndex, snapshot.leftChildNodeRank, snapshot.rightChildIndex, snapshot.rightChildNodeRank);
	if (! _is_root) {
		LOGGER(_sweeplogger,V4_VVER, "SWEEP [%i] BCAST RESET non-root \n", _my_rank);
		//Prepare all non-root processes to be ready to receive the next broadcast
		//CRUCIAL: use getJobTree().getSnapshot() instead of snapshot !! Otherwise we would re-use the same old outdated snapshot for eternity
		_bcast.reset(new JobTreeBroadcast(getId(), getJobTree().getSnapshot(), [this]() {cbContributeToAllReduce();}, TAG_BCAST_INIT));
		if (getJobTree().getCommSize() < getVolume()) {
			LOGGER(_sweeplogger,V1_WARN, ">>>> WARN SWEEP [%i] BCAST Tree size %i smaller than volume %i \n", _my_rank, getJobTree().getCommSize(), getVolume());
		}
	}
	if (_terminate_all.load(std::memory_order_relaxed)) {
		LOGGER(_sweeplogger,V4_VVER, "SWEEP BCAST SKIP reduction, status is already _terminate_all\n");
		return;
	}
	if (_red && _red->hasResult()) {
		LOGGER(_sweeplogger,V3_VERB, ">>>> WARN SWEEP [%i] Noticing unextracted _red results late during broadcast callback\n", _my_rank);
		extractAllReductionResult();
	}
	JobMessage baseMsg = getMessageTemplate();
	baseMsg.tag = TAG_ALLRED;
	LOGGER(_sweeplogger,V4_VVER, "SWEEP [%i] RED SHARE RESET\n", _my_rank);
	_red.reset(new JobTreeAllReduction(snapshot, baseMsg, std::vector<int>(), aggregateEqUnitContributions));
	if (_is_root)
		_red->setInplaceTransformationOfElementAtRoot(_inplace_rootTransform);
	//Bring individual data per thread in the sharing element format
	std::list<std::vector<int>> contribs;
	int id=-1; //for debugging
	for (auto &sweeper : _sweepers) {
		id++;
		if (!sweeper) {
			LOGGER(_sweeplogger,V5_DEBG, "SWEEP [%i](%i) not yet initialized, skipped in contribution aggregation \n", _my_rank, id);
			continue;
		}
		//Mutex, because the solvers are asynchronously pushing all the time new eqs&units into these vectors
		//(including reallocations after push_back), make sure that doesn't happen while we copy/move
		std::vector<int> eqs, units;
		{
			std::lock_guard<std::mutex> lock(sweeper->sweep_export_mutex);
			eqs = std::move(sweeper->eqs_to_share);
			units = std::move(sweeper->units_to_share);
			if (!eqs.empty() || !units.empty()) {
				LOGGER(_sweeplogger,V5_DEBG, "        [%i](%i) Export:  %i E  %i U\n", _my_rank, sweeper->getLocalId(), eqs.size()/2, units.size());
			}
			//by moving we also clear their current position, i.e. prevents from sharing the data twice
		}
		Metadata md;
		md.eq_size = eqs.size();
		md.unit_size = units.size();
		assert(md.eq_size%2==0 || log_return_false("ERROR in AGGR: Non-even number %i of equivalence literals, should always come in pairs", md.eq_size));
		LOGGER(_sweeplogger,V5_DEBG, "SWEEP SHARE REDUCE (%i): %i eq_size, %i units, %i idle \n", sweeper->getLocalId(), md.eq_size, md.unit_size, sweeper->sweeper_is_idle);
		_rank_contributed_equalities += md.eq_size/2;
		_rank_contributed_units += md.unit_size;
		//Format: [eqs, units, metadata]
		std::vector<int> contrib = std::move(eqs);
		contrib.insert(contrib.end(), units.begin(), units.end());
		auto stats = sweeper->fetchSweepStats();
		md.idle_count = sweeper->sweeper_is_idle;
		md.longtermidle_count = sweeper->sweeper_longterm_idle;
		md.active_count = !md.idle_count;
		// md.working_internally_count = shweep_working_internally(sweeper->solver);
		md.foundUnsat = kissat_is_inconsistent(sweeper->solver);
		md.work_sweeps = (int)stats.progress_work_sweeps;
		md.work_stepovers = (int)stats.progress_work_stepovers;
		md.unsched_resweeps = (int)stats.progress_unsched_resweeps;
		md.maxxed_kittens = stats.maxxed_kittens;
		md.lagging = isSolverLagging(sweeper);
		md.sweeper_objs = shweep_has_sweeper_obj(sweeper->solver);
		//Steal amount can overestimate the actual remaining work, but is quick and cheap to calculate
		//*2, because stealing considers half of the available work
		md.remaining_work_estimate = 2*shweep_get_max_steal_amount(sweeper->solver);
		if (stats.local_iteration < expected_iteration_of_next_round) {
			//edgecase: This sweeper is still stuck in a previous iteration, maybe because its last Kitten Call
			//takes extremely long, and it didn't yet reach the point where it resets it's work statistics
			//we catch that case here and filter out "left-over" statistics form the previous iteration
			md.work_sweeps = 0;
			md.work_stepovers = 0;
			md.unsched_resweeps = 0;
		}
		appendMetadataToReductionElement(contrib, md);
		contribs.push_back(contrib);
	}
	auto aggregation_element = aggregateEqUnitContributions(contribs);
	LOGGER(_sweeplogger,V4_VVER, "SWEEP [%i] contributing ~~~%zu~~~(+%i)~~> to _red \n", _my_rank, aggregation_element.size()-NUM_METADATA_FIELDS, NUM_METADATA_FIELDS);
	if (_terminate_all.load(std::memory_order_relaxed)) {
		LOGGER(_sweeplogger,V4_VVER, "SWEEP SHARE BCAST skip contribution, seen already _terminate_all\n");
		return;
	}
	_timestamp_contributed_to_sharing.push_back(Timer::elapsedSeconds());
	_red->contribute(std::move(aggregation_element));
}

void SweepJob::advanceAllReduction() {
	if (!_red)
		return;
	//always keep the global reduction advancing, independently of the state of the local solvers
	_red->advance();
	if (_red->hasResult()) {
		extractAllReductionResult();
	}
}

void SweepJob::extractAllReductionResult() {
	//There is data from global aggregation, extract it
	assert(_red);
	assert(_red->hasResult());
	auto data = _red->extractResult();
	const Metadata md = readMetadataFromReductionElement(data);
	assert(md.eq_size%2==0 || log_return_false("SWEEP ERROR: Import Equality size %i not even\n", md.eq_size));
	_timestamp_receive_sharing_result.push_back(Timer::elapsedSeconds());
	_rank_is_inbetween_iterations = md.end_iteration;
	_iteration_of_round[md.sharing_round] = md.sweep_iteration;
	expected_iteration_of_next_round = md.sweep_iteration;
	if (md.end_iteration) {
		expected_iteration_of_next_round += 1;
	}
	bool all_idle = (md.active_count==0);
	if (_is_root) {
		LOGGER(_sweeplogger,V4_VVER, "SWEEP GOTT: iter %i round %i : %i ai , %i endi , %i trm . act,idle %i,%i   E %i  U %i  \n", md.sweep_iteration, md.sharing_round, all_idle, md.end_iteration, md.terminate, md.active_count, md.idle_count, md.eq_size/2, md.unit_size);
	}
	assert(md.sharing_round > _lastImportedRound.load() || log_return_false("SWEEP ERROR : unexpected round number when importing shared data. got round %i, while lastImportedRound %i \n", md.sharing_round, _lastImportedRound.load()));
	assert(_imported_data[md.sharing_round].eqs.empty()   || log_return_false("SWEEP ERROR : want to store %i shared eq   integers, but already importedRounds[%i].eqs.size()==%zu nonempty ", md.eq_size,  md.sharing_round, _imported_data[md.sharing_round].eqs.size()));
	assert(_imported_data[md.sharing_round].units.empty() || log_return_false("SWEEP ERROR : want to store %i shared unit integers, but already importedRounds[%i].units.size()==%zu nonempty", md.unit_size,  md.sharing_round, _imported_data[md.sharing_round].units.size()));
	if (md.sharing_round >= MAX_IMPORT_ROUNDS) {
		LOGGER(_sweeplogger,V0_CRIT, "SWEEP ERROR : reached hardcoded limit of %i Sharing rounds. Increase that limit if needed for your case.\n", MAX_IMPORT_ROUNDS);
	}
	_imported_data[md.sharing_round].eqs   = std::vector<int>(data.begin()             , data.begin() + md.eq_size);
	_imported_data[md.sharing_round].units = std::vector<int>(data.begin() + md.eq_size, data.begin() + md.eq_size + md.unit_size);
	_lastImportedRound = md.sharing_round;
	if (md.foundUnsat) {
		//A bit paranoid, but Eqs&Units are no longer relevant after we found Unsat. So we don't even pass them to the sweepers anymore.
		_imported_data[md.sharing_round].eqs   = {};
		_imported_data[md.sharing_round].units = {};
	}
	//the root node is special in that it is the only node that initiates sharing rounds.
	//Prepare for a new one, since we just extracted all the shared data from the current round.
	if (_is_root) {
		LOGGER(_sweeplogger,V4_VVER, "SWEEP root: RESET BCAST for next sharing round\n", _my_rank);
		_bcast.reset(new JobTreeBroadcast(getId(), getJobTree().getSnapshot(),
			[this]() {cbContributeToAllReduce();}, TAG_BCAST_INIT));
	}

	//Reduction is finished. we dont need to directly re-create a new reduction object,
	//but can leave it at null (Contrary to the bcast)
	//The new reduction object will be created by the next bcast round when needed
	_red.reset();

	//Some solvers can "lag behind" in iterations, if their initial formula loading takes so long that they
	//arrive at sweeping when the global sweeping already finished the first iteration
	//Thus, tell them which iteration we are in
	if (_flag_started_synchronized_solving  && !_terminate_all) {
		for (auto &sweeper : _sweepers) {
			if (sweeper) {
				shweep_set_global_iteration(sweeper->solver, md.sweep_iteration);
			}
		}
	}


	//Tell the solvers that the iteration ended
	if (md.end_iteration && _flag_started_synchronized_solving  && !_terminate_all) {
		LOGGER(_sweeplogger,V4_VVER, "sending end_iteration signal to solvers\n");
		for (auto &sweeper : _sweepers) {
			if (sweeper) {
				shweep_set_end_iteration_signal(sweeper->solver);
			}
		}
	}

	//Check whether the whole sweep job sould be terminated.
	//We do this check chronologically last in this function, because there might still be useful shared data
	//that we want to import before terminating, and having an earlier termination signal only increases risks for concurrency problems
	if (md.terminate) {
		_terminate_all = true;
		triggerTerminations();
		LOGGER(_sweeplogger,V2_INFO, "# \n # \n # --- [%i] got terminate flag, TERMINATING SWEEP JOB ---\n # \n", _my_rank);
		LOG(V2_INFO, "SweepJob got [%i] got terminate flag\n", _my_rank);
	}
}

void SweepJob::clearImportedRound() {
	//We store data from multiple past import rounds, as long as some threads have not imported them yet.
	//Eventually we want and should delete this data, as it consumes (some small) memory.
	//We delete a round data once all its  Eqs and Units have been imported by all threads
	//This function only clears one round per invocation, this suffices since its called more often than new rounds are added
	int r = _lastClearedRound + 1; //try to clear the next stored and uncleared round
	assert(_finishedRoundCounters[r].threads_finished_eqs   <= _nThreads);
	assert(_finishedRoundCounters[r].threads_finished_units <= _nThreads);
	if (_finishedRoundCounters[r].threads_finished_eqs == _nThreads && _finishedRoundCounters[r].threads_finished_units == _nThreads) {
		_imported_data[r].eqs.clear();
		_imported_data[r].units.clear();
		LOGGER(_sweeplogger,V4_VVER, "SWEEP [%i] CLEARED round %i data \n", _my_rank, r);
		_lastClearedRound = r;
	}
}

bool SweepJob::isSolverLagging(KissatPtr sweeper) {
	//A solver is considered lagging if it is more than MIN_LAGGING_ROUNDS rounds behind in importing equalities and units
	//This happens when a solver is stuck for multiple seconds within a single -particularly hard- sweep call
	constexpr int MIN_LAGGING_ROUNDS = 50;
	return sweeper->curr_eq_round < (_lastImportedRound.load() - MIN_LAGGING_ROUNDS);
}

int SweepJob::countLaggingSolvers() {
	if (_terminate_all) {
		return 0;
	}
	int lagging = 0;
	for (auto &sweeper : _sweepers) {
		if (sweeper && isSolverLagging(sweeper)) {
			lagging++;
			uint64_t kitten_propagations = shweep_kitten_propagations(sweeper->solver);
			const char *profile = shweep_get_profilename(sweeper->solver);
			auto stats = sweeper->fetchSweepStats();
			if (_is_root) {
				LOGGER(_sweeplogger,V3_VERB, "WARN [%i](%i) lags %i vs %i (%i). kcalls %i  kprops %zu  kprof %s iter %i\n",
					_my_rank, sweeper->getLocalId(), sweeper->curr_eq_round, _lastImportedRound.load(), sweeper->curr_eq_round - _lastImportedRound.load(), stats.kitten_calls, kitten_propagations, profile, stats.local_iteration);
			}
		}
	}
	return lagging;

}

std::vector<int> SweepJob::aggregateEqUnitContributions(std::list<std::vector<int>> &contribs) {
	//sanity check whether each contribution contains coherent size information about itself
	for (const auto& contrib : contribs) {
		const Metadata md = readMetadataFromReductionElement(contrib);
		int claimed_total_size = md.eq_size + md.unit_size + NUM_METADATA_FIELDS;
		assert(contrib.size() == claimed_total_size ||
			log_return_false("ERROR in AllReduce, Bad Element Format: Claims total size %i != %zu actual contrib.size() (claims: eq_size %i,  units size %i, metadata %i)",
				claimed_total_size, contrib.size(), md.eq_size, md.unit_size, NUM_METADATA_FIELDS)
			);
	}

	//Layout: contrib = [equivalences, units, metadata]

	//Deduplicate
	robin_hood::unordered_flat_set<int> set_of_units;
	robin_hood::unordered_flat_set<uint64_t> set_of_eqs;
	for (const auto &contrib : contribs) {
		const Metadata md = readMetadataFromReductionElement(contrib);
		const int eq_size = md.eq_size;
		const int unit_size = md.unit_size;
		//Deduplicate Equivalences
		//each eq-pair is represented as one 64-bit key, to have an easy datatype
		for (int j=0; j < eq_size; j+=2) {
			int elit1 = contrib[j];
			int elit2 = contrib[j+1];
			assert(std::abs(elit1) < std::abs(elit2) || log_return_false("SWEEP ERROR: in aggregate: abs(elit1) is larger than abs(elit2), but it should be smaller. elit1=%i, elit2=%i, at index j=%i\n", elit1, elit2, j));
			// The uint32_t cast is critically necessary to truncate to 32 bits first,
			// killing the sign extension. Only this way we can pack it without mangling the values
			uint64_t litpair = (((uint64_t)(uint32_t)elit1) << 32) |  ((uint64_t)(uint32_t)elit2);
			set_of_eqs.insert(litpair);
		}
		//Deduplicate Units
		const int units_start = eq_size;
		const int units_end   = units_start + unit_size;
		for (int j=units_start; j< units_end; j++) {
			int32_t eunit = contrib[j];
			set_of_units.insert(eunit);
		}
	}

	//Extract the deduplicated equivalences and units
	std::vector<int> aggregated;
	int total_aggregated_size = 0;
	//each equivalence-key is 64 bit, i.e. contains two 32-bit literals
	int aggr_eq_size   = 2*set_of_eqs.size();
	int aggr_unit_size = set_of_units.size();
	int aggr_data_size = aggr_eq_size + aggr_unit_size;
	aggregated.resize(aggr_data_size);
	int j=0;
	for (uint64_t litpair : set_of_eqs) {
		//Extraction: explicit narrowing cast to recover sign
		int elit1 = (int32_t)(litpair >> 32);
		int elit2 = (int32_t)(litpair & 0xffffffff);
		assert(std::abs(elit1) < std::abs(elit2) || log_return_false("SWEEP ERROR: in extraction on aggregate: abs(elit1) is larger than abs(elit2), but it should be smaller. elit1=%i, elit2=%i, at index j=%i\n", elit1, elit2, j));
		aggregated[j++] = elit1;
		aggregated[j++] = elit2;
	}
	for (int unit : set_of_units) {
		aggregated[j++] = unit;
	}
	assert(j==aggr_data_size);
	total_aggregated_size = aggr_data_size + NUM_METADATA_FIELDS;

	//Aggregate additional metadata fields by summing each contribution's tail
	Metadata md;
	md.eq_size = aggr_eq_size;
	md.unit_size = aggr_unit_size;
	for (const auto &contrib : contribs) {
		const Metadata c = readMetadataFromReductionElement(contrib);
		md.idle_count				+= c.idle_count;
		md.longtermidle_count		+= c.longtermidle_count;
		md.active_count				+= c.active_count;
		md.foundUnsat				+= c.foundUnsat;
		md.maxxed_kittens			+= c.maxxed_kittens;
		md.working_internally_count += c.working_internally_count;
		md.work_sweeps				+= c.work_sweeps;
		md.work_stepovers			+= c.work_stepovers;
		md.unsched_resweeps			+= c.unsched_resweeps;
		md.lagging                  += c.lagging;
		md.remaining_work_estimate  += c.remaining_work_estimate;
		md.sweeper_objs				+= c.sweeper_objs;
	}
	if (contribs.empty()) {
		//edge-case: if not a single solver is initialized yet,
		//we are waiting for them to come online, so they are not really idle
		if (md.active_count==0) {
			md.active_count=1;
		}
	}
	appendMetadataToReductionElement(aggregated, md);
	LOG(V5_DEBG, "SWEEP RED aggregated %i contributions: E %i, U %i, act,idle %i,%i\n", contribs.size(), aggr_eq_size/2, aggr_unit_size, md.active_count, md.idle_count);
	int individual_sum =  aggr_eq_size + aggr_unit_size + NUM_METADATA_FIELDS;
	assert(total_aggregated_size == individual_sum ||
		log_return_false("SWEEP ERROR: aggregated element assert failed: total_size %i != %i individual_sum (total_eq_size %i + total_unit_size %i + metadata %i) ",
			total_aggregated_size, individual_sum, aggr_eq_size, aggr_unit_size, NUM_METADATA_FIELDS));
    return aggregated;
}



std::vector<int> SweepJob::stealWorkFromAnyLocalSolver(int asking_rank, int asking_sourceLocalId) {
	//Parameters are only used for verbose logging, don't influence function behaviour
	auto rand_permutation = getRandomIdPermutation();
	for (int localId : rand_permutation) {
		auto stolen_work = stealWorkFromSpecificLocalSolver(localId);
		if ( ! stolen_work.empty()) {
			LOGGER(_sweeplogger,V4_VVER, "SWEEP giv [%i](%i) ===%zu===> [%i](%i) \n",_my_rank, localId, stolen_work.size(), asking_rank, asking_sourceLocalId);
			return stolen_work;
		}
	}
	//no work available at the local rank
	return {};
}

//Syntatic sugar helper, to automatically clear a flag when it goes out of scope, so we don't have to do it manually
struct scoped_guard {
    std::atomic_flag& flag;
    bool active;
    scoped_guard(std::atomic_flag& f) : flag(f), active(!f.test_and_set(std::memory_order_acquire)) {}
    ~scoped_guard() {
        if (active)
            flag.clear(std::memory_order_release);
    }
    bool acquired() const { return active; }
};


std::vector<int> SweepJob::stealWorkFromSpecificLocalSolver(int localId) {
	if (_terminate_all.load(std::memory_order_relaxed))
		//sweeping finished globally, nothing to steal anymore
		return {};
	if ( ! _sweepers[localId]) {
		return {};
	}
	KissatPtr sweeper = _sweepers[localId];
	if ( ! sweeper->solver) {
		return {};
	}
	//We dont know yet how much there is to steal, so we ask for an upper bound
	//It can also be that the solver we want to steal from is not fully initialized yet
	//For that in the C code there are further guards against unfinished initialization, all returning 0 in that case
	//The congruence closure solver will always return 0, as it doesnt operate on work and doesnt have any
	int max_steal_amount = shweep_get_max_steal_amount(sweeper->solver);
	if (max_steal_amount < MIN_STEAL_AMOUNT)
		return {};
	assert(max_steal_amount > 0			 || log_return_false("SWEEP STEAL ERROR [%i](%i): negative max steal amount %i, maybe segfault into non-initialized kissat solver \n", _my_rank, localId, max_steal_amount));
	assert(max_steal_amount < 2*_numVars || log_return_false("SWEEP STEAL ERROR [%i](%i): too large max steal amount %i >= 2*NUM_VARS, maybe segfault into non-initialized kissat solver \n", _my_rank, localId, max_steal_amount));
	//There is something to steal.
	//Use mutex to prevent multiple solvers from stealing concurrently from the same solver.
	//While this is a sane choice in general, it also seemed that 23 threads stealing at the same time from one solver
	//actually delayed the first stealing by some ~20-30ms compared to only a single solver stealing.
	//maybe 23 threads reading and writing concurrently from the same worklist created strong cacheline management pressure...
	scoped_guard steal_guard(sweeper->steal_victim_lock);
	if (!steal_guard.acquired())
		return {};

	//Allocate memory for the steal here in C++, and pass the allocation to kissat for filling
	std::vector<int> stolen_work = std::vector<int>(max_steal_amount);
	int actually_stolen = shweep_steal_from_this_solver(sweeper->solver, reinterpret_cast<unsigned int*>(stolen_work.data()), max_steal_amount);
	assert(actually_stolen >= 0 || log_return_false("SWEEP ERROR : negative stolen amount %i \n", actually_stolen));
	//We allocated the steal array as large as maximally needed, but that was only an upper limit estimate,
	//often during stealing it turns out that we receive a bit less work than estimated
	//So now we know the exact amount of work we stole,
	//and shrink the array to have its .size() match with this stolen amount
	stolen_work.resize(actually_stolen);

	return stolen_work;
	//lock is freed up now automatically by going out of scope
}

void SweepJob::printActiveMPIRequestsCount() {
	int active=0;
	for (auto &request : _worksteal_requests) {
		active+=request.is_active;
	}
	LOGGER(_sweeplogger,V3_VERB, "still active MPI requests: %i\n",active);
}

std::vector<int> SweepJob::getRandomIdPermutation() {
	auto permutation = _list_of_ids; //copy
	//created/seeded only once per thread, then just advancing rng calls
	static thread_local std::mt19937 rng(std::random_device{}());
	std::shuffle(permutation.begin(), permutation.end(), rng);
	return permutation;
}

void SweepJob::crossjob_rootReceiveClauses(std::vector<int>  &&clauses) {
	if (!_params.sweepXTCSrecv()) {
		return;
	}
	LOGGER(_sweeplogger,V4_VVER, "SWEEP storing part of received XTCS size %i\n",clauses.size());
	auto reader = BufferReader(clauses.data(), clauses.size(), 10, false);
	auto clause = reader.getNextIncomingClause();
	{
		int before = _crossjob_root_received_units.size();
		std::lock_guard<std::mutex> lock(_crossjob_import_mutex);
		while (clause.begin != nullptr) {
			if (ClauseMetadata::enabled()) {
				assert(clause.size >= ClauseMetadata::numInts()+1 || log_return_false("[ERROR] Clause of invalid size %i!\n", clause.size));
				uint64_t id;
				memcpy(&id, clause.begin, sizeof(uint64_t));
			}
			if (clause.size==1) {
				assert(clause.begin[0]!=0 || log_return_false("SWEEPsns ERROR : got crossjob unit literal 0\n"));
				_crossjob_root_received_units.push_back(clause.begin[0]);
			}
			//Technically we could also scan for equivalences (in linear time),
			//but for now not added this extraction algorithm
			clause = reader.getNextIncomingClause();
		}
		int after = _crossjob_root_received_units.size();
		LOGGER(_sweeplogger,V4_VVER, "SWEEP stored %i received XTCS units \n", after -before);
	}
}


void SweepJob::loadFormula(KissatPtr sweeper) {
	const int* lits = getDescription().getFormulaPayload(0);
	const int payload_size = getDescription().getFormulaPayloadSize(0);
	constexpr int BITS_PER_MB = 8000000;
	float formula_in_MB = ((float)payload_size*32)/BITS_PER_MB;
	LOGGER(_sweeplogger,V3_VERB, "SWEEP [%i](%i) loading formula (%.3f MB) \n", _my_rank, sweeper->getLocalId(), formula_in_MB);
	float t0 = Timer::elapsedSeconds();
	constexpr int CHECK_INTERVAL = 50000;
	int counter = CHECK_INTERVAL;
	for (int i = 0; i < payload_size; i++) {
		sweeper->addLiteral(lits[i]);
		if (--counter == 0) {
			counter = CHECK_INTERVAL;
			if (_terminate_all.load(std::memory_order_relaxed)) {
				LOGGER(_sweeplogger,V1_WARN, "SWEEP WARN [%i](%i) stopped loading formula due to termination  (at payload lit %i / %i) \n", _my_rank, sweeper->getLocalId(), i, payload_size);
				break;
			}
		}
	}
	float t1 = Timer::elapsedSeconds();
	LOGGER(_sweeplogger,V3_VERB, "SWEEP [%i](%i) loaded formula (%.3f MB) in %.6f sec \n", _my_rank, sweeper->getLocalId(), formula_in_MB , (t1-t0));
}

void SweepJob::triggerTerminations() {
	LOGGER(_sweeplogger,  V2_INFO, "SWEEP TERM #%i [%i] trigger solver terminations (ctx %i). Of: Running %i, Finished %i \n", getId(), _my_rank, _my_ctx_id, _running_sweepers_count.load(), _finished_sweepers_count.load());
	LOG   (               V2_INFO, "SWEEP TERM #%i [%i] trigger solver terminations (ctx %i). Of: Running %i, Finished %i \n", getId(), _my_rank, _my_ctx_id, _running_sweepers_count.load(), _finished_sweepers_count.load());
	printActiveMPIRequestsCount();
	int i=0;
	for (auto &sweeper : _sweepers) {
		if (sweeper) {
			sweeper->triggerSweepTerminate();
			LOGGER(_sweeplogger,                V3_VERB, "SWEEP TERM #%i [%i] trigger termination of solver (%i) \n", getId(), _my_rank, i);
		} else {
			LOGGER(_sweeplogger,				V3_VERB, "SWEEP TERM #%i [%i] skip    termination of solver (%i), already null \n", getId(), _my_rank, i);
		}
		i++;
	}
}

SweepJob::~SweepJob() {
	LOGGER(_sweeplogger,V2_INFO, "SWEEP JOB DESTRUCTOR ENTERED (ctx %i) \n", _my_ctx_id);
	LOG(                V2_INFO, "SWEEP JOB DESTRUCTOR ENTERED (ctx %i) \n", _my_ctx_id);
	for (int i=0; i<5; i++) {
		clearImportedRound();
	}
	if (_flag_terminated_while_synchronizing) {
		LOGGER(_sweeplogger,V1_WARN, "SWEEP [%i] WARN : rank was terminated while synchronizing \n", _my_rank);
	}
	if (!_flag_terminated_while_synchronizing && (_lastClearedRound + 2 < _lastImportedRound)) {
		LOGGER(_sweeplogger,V1_WARN, "SWEEP [%i] WARN : didn't clear all imported rounds. lastCleared %i, lastImported %i \n", _my_rank, _lastClearedRound, _lastImportedRound.load());
	}
	if (_lastImportedRound==0) {
		LOGGER(_sweeplogger,V1_WARN, "SWEEP [%i] WARN : rank didn't receive a single sharing round! (irrelevant if only 1 rank present) \n", _my_rank);
	}
	// triggerTerminations();
	LOGGER(_sweeplogger,V2_INFO, "SWEEP JOB DESTRUCTOR DONE ctx %i\n", _my_ctx_id);
	LOG(                V2_INFO, "SWEEP JOB DESTRUCTOR DONE ctx %i\n", _my_ctx_id);
}




