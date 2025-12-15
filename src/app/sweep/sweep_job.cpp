#include "sweep_job.hpp"

#include "app/job.hpp"
#include "app/job_tree.hpp"
#include "util/ctre.hpp"
#include "util/logger.hpp"
#include "util/sys/tmpdir.hpp"

extern "C" {
#include "kissat/src/kissat.h"
}


SweepJob::SweepJob(const Parameters& params, const JobSetup& setup, AppMessageTable& table)
    : Job(params, setup, table), _reslogger(Logger::getMainInstance().copy("<RESULT>", ".sweep"))
{
	assert(_params.jobCommUpdatePeriod() > 0 || log_return_false("[ERROR] For this application to work,"
            " you must explicitly enable job communicators with the -jcup option, e.g., -jcup=0.1\n"));
	LOG(V2_INFO, "New SweepJob MPI Process rank %i with %i threads\n", getJobTree().getRank(), params.numThreadsPerProcess.val);
}


//callback from kissat
void cb_search_work_in_tree(void *SweepJob_state, unsigned **work, int *work_size, int local_id) {
    ((SweepJob*) SweepJob_state)->cbSearchWorkInTree(work, work_size, local_id);
}

void cb_import_eq(void *SweepJobState, int *lit1, int *lit2, int localId) {
	((SweepJob*) SweepJobState)->cbImportEq(lit1, lit2, localId);
}

void cb_import_unit(void *SweepJobState, int *lit, int localId) {
	((SweepJob*) SweepJobState)->cbImportUnit(lit, localId);
}

int cb_custom_query(void *SweepJobState, int query) {
	return ((SweepJob*)SweepJobState)->cbCustomQuery(query);
}

void SweepJob::appl_start() {
	_my_rank = getJobTree().getRank();
	_my_index = getJobTree().getIndex();
	_is_root = getJobTree().isRoot();
	_nThreads = _params.numThreadsPerProcess.val;
	LOG(V2_INFO,"SWEEP JOB SweepJob appl_start() STARTED: Rank %i, Index %i, ContextId %i, is root? %i, Parent-Rank %i, Parent-Index %i, threads=%d\n",
		_my_rank, _my_index, getJobTree().getContextId(), _is_root, getJobTree().getParentNodeRank(), getJobTree().getParentIndex(), _nThreads);
	// LOG(V2_INFO,"SWEEP JOB sweep-sharing-period: %i ms\n", _params.sweepSharingPeriod_ms.val);
	// LOG(V2_INFO, "New SweepJob rank %i working on %i vars in %i clauses \n", getJobTree().getRank(), );
    _metadata = getSerializedDescription(0)->data();
	_start_sweep_timestamp = Timer::elapsedSeconds();
	_last_sharing_start_timestamp = Timer::elapsedSeconds();

    _reslogger = Logger::getMainInstance().copy("<RESULT>", ".sweep");

	//do not trigger a send on the initial dummy worksteal requests
	_worksteal_requests.resize(_nThreads);
	for (auto &request : _worksteal_requests) {
		request.sent = true;
	}

	//pre-allocate a fixed array from where solver can concurrently import the received equalities and units
	_EQS_to_import.resize(MAX_IMPORT_SIZE);
	_UNITS_to_import.resize(MAX_IMPORT_SIZE);

	_worksweeps = std::vector<int>(_nThreads, -1);
	_resweeps_in = std::vector<int>(_nThreads, -1);
	_resweeps_out = std::vector<int>(_nThreads, -1);

	//To randomize workstealing on a rank level, we will shuffle the localIds to determine read order
	for (int localId=0; localId < _nThreads; ++localId) {
		_list_of_ids.push_back(localId);
	}

	//Will hold pointers to the kissat solvers
	_sweepers.resize(_nThreads);

	//Initialize the background workers, each will run one kissat thread
	_bg_workers.reserve(_nThreads);
	for (int i = 0; i < _nThreads; ++i) {
		_bg_workers.emplace_back(std::make_unique<BackgroundWorker>());
	}

	_bcast.reset(new JobTreeBroadcast(getId(), getJobTree().getSnapshot(), [this]() {cbContributeToAllReduce();}, TAG_BCAST_INIT));

	//Start individual Kissat threads (those then immediately jump into the sweep algorithm)
	LOG(V2_INFO,"SWEEP JOB Create solvers\n");
	for (int localId=0; localId < _nThreads; localId++) {
		createAndStartNewSweeper(localId);
	}

	//might as well already set some result metadata information now
	_internal_result.id = getId();
	_internal_result.revision = getRevision();

	LOG(V2_INFO, "SWEEP JOB appl_start() FINISHED\n");
	// _finished_job_setup = true;
}



// Called periodically by the main thread to allow the worker to emit messages.
void SweepJob::appl_communicate() {
	LOG(V4_VVER, "SWEEP JOB appl_communicate() \n");

	printIdleFraction();
	sendMPIWorkstealRequests();
	checkForUnsatResults();
	if (_bcast && _is_root)// Root: Update job tree snapshot in case your children changed
		_bcast->updateJobTree(getJobTree());

	if (_is_root && ! _terminate_all)
		initiateNewSharingRound();

	advanceAllReduction();
}


// React to an incoming message. (This becomes relevant only if you send custom messages)
void SweepJob::appl_communicate(int sourceRank, int mpiTag, JobMessage& msg) {
	// LOG(V2_INFO, "Shweep rank %i: received custom message from source %i, mpiTag %i, msg.tag %i \n", _my_rank, source, mpiTag, msg.tag);
	if (mpiTag != MSG_SEND_APPLICATION_MESSAGE) {
		LOG(V1_WARN, "SWEEP MSG Warn [%i] got unexpected message with mpiTag=%i, msg.tag=%i  (instead of MSG_SEND_APPLICATION_MESSAGE mpiTag == 30)\n", _my_rank, mpiTag, msg.tag);
	}
	if (msg.returnedToSender) {
		LOG(V1_WARN, "SWEEP MSG Warn [%i]: received unexpected returnedToSender message during Sweep Job Workstealing! source=%i mpiTag=%i, msg.tag=%i treeIdxOfSender=%i, treeIdxOfDestination=%i \n", _my_rank, sourceRank, mpiTag, msg.tag, msg.treeIndexOfSender, msg.treeIndexOfDestination);
		// assert(log_return_false("SWEEP MSG ERROR, got msg.returnToSender"));
	}
	else if (msg.tag == TAG_SEARCHING_WORK) {
		assert(msg.payload.size() == 1);
		int localId = msg.payload.front();
		msg.payload.clear();

		LOG(V3_VERB, "SWEEP MSG [%i] <---?---- [%i](%i) \n", _my_rank, sourceRank, localId);
		auto locally_stolen_work = stealWorkFromAnyLocalSolver(sourceRank, localId);

		msg.payload = std::move(locally_stolen_work);
		msg.payload.push_back(localId);

		//send back to source
		msg.tag = TAG_RETURNING_STEAL_REQUEST;
		int sourceIndex = getJobComm().getInternalRankOrMinusOne(sourceRank);
		msg.treeIndexOfDestination = sourceIndex;
		msg.contextIdOfDestination = getJobComm().getContextIdOrZero(sourceIndex);
		assert(msg.contextIdOfDestination != 0 ||
			log_return_false("SWEEP STEAL ERROR in TAG_RETURNING_STEAL_REQUEST! Want to return an message, but invalid contextIdOfDestination==0. "
					"With sourceRank=%i, sourceIndex=%i, payload.size()=%i \n", sourceRank, sourceIndex, msg.payload.size()));

		getJobTree().send(sourceRank, MSG_SEND_APPLICATION_MESSAGE, msg);
	}
	else if (msg.tag == TAG_RETURNING_STEAL_REQUEST) {
		int localId = msg.payload.back();
		msg.payload.pop_back();
		_worksteal_requests[localId].stolen_work = std::move(msg.payload);
		_worksteal_requests[localId].got_steal_response = true;
		if (_worksteal_requests[localId].stolen_work.size() > 0)
			LOG(V3_VERB, "SWEEP MSG [%i](%i) <==%i==== [%i]\n", _my_rank, localId, _worksteal_requests[localId].stolen_work.size(), sourceRank );
		else
			LOG(V3_VERB, "SWEEP MSG [%i](%i) <---0---- [%i]\n", _my_rank, localId, _worksteal_requests[localId].stolen_work.size(), sourceRank );
	}
	else if (msg.tag == TAG_FOUND_UNSAT) {
		LOG(V1_WARN, "SWEEP MSG [%i] <~~~ Found UNSAT! [%i]\n", _my_rank, sourceRank );
		assert(_is_root);
		bool expected = false;
		//report exactly once to Mallob, ignore all additional internal UNSAT messages
		if (_root_reported_unsat.compare_exchange_strong(expected, true)) {
			reportSolverResult(nullptr, UNSAT);
		}

	}
	else if (mpiTag == MSG_NOTIFY_JOB_ABORTING)    {LOG(V1_WARN, "SWEEP MSG Warn [%i]: received NOTIFY_JOB_ABORTING \n", _my_rank);}
	else if (mpiTag == MSG_NOTIFY_JOB_TERMINATING) {LOG(V1_WARN, "SWEEP MSG Warn [%i]: received NOTIFY_JOB_TERMINATING \n", _my_rank);}
	else if (mpiTag == MSG_INTERRUPT)			   {LOG(V1_WARN, "SWEEP MSG Warn [%i]: received MSG_INTERRUPT \n", _my_rank);}
	else {LOG(V1_WARN, "SWEEP MSG Warn [%i]: received unexpected mpiTag %i with msg.tag %i \n", _my_rank, mpiTag, msg.tag);}
}

void SweepJob::appl_terminate() {
	LOG(V2_INFO, "SWEEP [%i] (job #%i) got TERMINATE signal (appl_terminate()) \n", _my_rank,getId());
	_terminate_all = true;
	_external_termination = true;
	gentlyTerminateSolvers();
}


void SweepJob::appl_memoryPanic() {
	LOG(V1_WARN, "[WARN] SWEEP [%i]: Memory panic! \n",_my_rank);
}

void SweepJob::checkForUnsatResults() {
	//It can be a mess when non-root node start to suddenly report (UNSAT) results to Mallob (maybe even concurrently), instead they only internaly inform the root node who then manages the reporting
	//Technically this message would only be needed to be send once, but for additional robustness it will just keep sending once UNSAT is found. Can also be a self-message.
	if (_do_report_UNSAT_to_root) {
		auto msg = getMessageTemplate();
		msg.tag = TAG_FOUND_UNSAT;
		getJobTree().sendToRoot(msg);
	}

}

void SweepJob::reportSolverResult(KissatPtr sweeper, int res) {
	LOG(V2_INFO, "SWEEP JOB [%i] reports sweep result %i to Mallob\n", _my_rank, res);
	assert(_is_root);
	assert(_solved_status == -1);
	std::vector<int> formula;
	if (res==UNSAT) {
		assert(sweeper == nullptr);
		formula = {};
	} else if (res==IMPROVED){
		assert(sweeper);
		formula = sweeper->extractPreprocessedFormula();
		LOG(V2_INFO, "SWEEP JOB [%i]: Solution Size %i\n", _my_rank, formula.size());
	} else {
		LOG(V1_WARN, "WARN SWEEP JOB [%i]: no improvement in final formula (result code %i) \n", _my_rank, res);
	}
	LOGGER(_reslogger,V2_INFO, "SWEEP_RESULT_CODE %i == %s \n", res, res==40 ? "IMPROVED" : res==20 ? "UNSATISFIABLE" : "UNKNOWN");
	_internal_result.setSolutionToSerialize(formula.data(), formula.size());
	_internal_result.result = res;
	_solved_status = res; //this change will be noticed by the main thread and triggers the termination of this job
}

void SweepJob::createAndStartNewSweeper(int localId) {
	LOG(V3_VERB, "SWEEP JOB [%i](%i) queuing background worker thread\n", _my_rank, localId);
	_bg_workers[localId]->run([this, localId]() {
		LOG(V3_VERB, "SWEEP JOB [%i](%i) WORKER START \n", _my_rank, localId);

		auto sweeper = createNewSweeper(localId);
		loadFormula(sweeper);
		_started_sweepers_count++; //only additive, monotonically increasing, going from 0...nThreads-1 and never decreased
		_running_sweepers_count++; //tracks actual number of running solvers at any given moment in time
		/*
		 *  Syncronization Layer!
		 *  We wait here until all other solvers are also initialized, and only then start solving
		 *  This is relevant for sweeping quality, as otherwise the solvers joining late might miss some of the equalities shared in the first rounds
		 *  Alternatively, store all the shared information as a warmup-greeting-package for newly joining solvers, to maximize quality. But only relevant if the SweepJob grows with time, which is not currently the case.
		 */
		while (_running_sweepers_count < _nThreads) {
			LOG(V3_VERB, "SWEEP JOB [%i](%i) waiting for other solvers to come online (%i/%i)\n", _my_rank, localId, _running_sweepers_count.load(), _nThreads);
			usleep(2000);
			if (_terminate_all || _external_termination) {
				LOG(V1_WARN, "Warn SWEEP [%i](%i): terminated while waiting in synchronization \n", _my_rank, localId);
				_running_sweepers_count--;
				return;
			}
		}
		_sweepers[localId] = sweeper; //only now expose the solver to the rest of the system, now that we know we start solving
		_started_synchronized_solving = true; //multiple threads will write non-thread-safe to this bool, but all only monotonically to "true"

		LOG(V3_VERB, "SWEEP JOB [%i](%i) solve() START \n", _my_rank, localId);
		int res = sweeper->solve(0, nullptr);
		LOG(V3_VERB, "SWEEP JOB [%i](%i) solve() FINISH. Result %i \n", _my_rank, localId, res);

		//transfer some solver-specific statistics
		auto stats = sweeper->fetchSweepStats();
		_worksweeps[localId] = stats.worksweeps;
		_resweeps_in[localId] = stats.resweeps_in;
		_resweeps_out[localId] = stats.resweeps_out;

		if (res==20) {
			//Found UNSAT
			assert(kissat_is_inconsistent(sweeper->solver) || log_return_false("SWEEP ERROR: Solver returned UNSAT 20 but is not in inconsistent (==UNSAT) state!\n"));
			_do_report_UNSAT_to_root = true;
			//if we are the very first solver to report anything, report and block others
			// todo: maybe easier to send quick MPI message to root node, instead of reporting directly here to Mallob? Would prevent double-reports more robustly...
			// bool expected = false;
			// if (_result_report_claimed.compare_exchange_strong(expected, true)) {
				// reportSolverResult(sweeper, UNSAT);
			// }
		} else if (res==0) {
			//There might be some progress in the formula, check.
			//To reduce concurrency problems, only a single dedicated solver (localId==0 on the root node) might have even reported a formula
			if (sweeper->hasPreprocessedFormula() ) { //by design the only sweeper that might have reported smth is the representative one with localId==0
				assert(_is_root);
				assert(sweeper->getLocalId()==0);
				//
				if (_root_reported_unsat) {
					LOG(V1_WARN, "SWEEP JOB [%i](%i): wanted to report improvement result, but stopped because other solvers already deduced UNSAT\n", _my_rank, localId);
				}  else {
					reportSolverResult(sweeper, IMPROVED);
				}
			}
		}

		//A dedicated solver on the root node print his stats as a representative of all other solvers.
		//Their stats differ slightly between solvers, but especially these global stats are very similar between all of them, so we don't bother aggregating/averaging them
		if (_is_root && localId == _representative_localId) {
			printSweepStats(sweeper, true);
		}

		assert(res==UNKNOWN || res==UNSAT || log_return_false("SWEEP ERROR: solver has returned with unexpected signal %i \n", res));
		//If no solver sets UNSAT or IMPROVED, the job will be returned by default as UNKNOWN

		_running_sweepers_count--;
		_sweepers[localId]->cleanUp(); //write kissat timing profile
		_sweepers[localId].reset();  //this should delete the only persistent shared pointer on the solver, and thus trigger its destructor soon now
		LOG(V3_VERB, "SWEEP JOB [%i](%i) WORKER EXIT\n", _my_rank, localId);

		// if (_localId==_representative_localId) {
		if (_running_sweepers_count==0) { //the last solver should report resweeps, as only then they are gathered from all exited solvers
			printResweeps();
		}
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
		setup.profilingBaseDir += "/" + std::to_string(_my_rank) + "/"; // rank == appRank ?
		// LOG(V4_VVER, "SWEEP [%i](%i) Profiling Dir = %s \n", _my_rank, localId, setup.profilingBaseDir.c_str());
		FileUtils::mkdir(setup.profilingBaseDir);
		setup.profilingLevel = _params.satProfilingLevel();
	}


	if (_numVars==0)
		_numVars = setup.numVars;

	// LOG(V2_INFO, "SWEEP JOB [%i](%i) create kissat shweeper \n", _my_rank, localId);
	float t0 = Timer::elapsedSeconds();
	std::shared_ptr<Kissat> sweeper(new Kissat(setup));
	float t1 = Timer::elapsedSeconds();
	float init_dur_ms =  (t1 - t0)*1000;
	const float WARN_init_dur = 50; //Usual initializations take 0.2ms in the Sat Solver Subprocess and 4-25ms  in the sweep job (for some weird reasons), but should never be above ~30ms
	LOG(V3_VERB, "SWEEP STARTUP [%i](%i) kissat init %f ms (%i/%i online)\n", _my_rank, localId, init_dur_ms, _running_sweepers_count.load(), _nThreads);
	if (init_dur_ms > WARN_init_dur) {
		LOG(V1_WARN, "SWEEP Warn STARTUP [%i](%i): kissat init took unusually long, %f ms !\n", _my_rank, localId, init_dur_ms);
	}

	sweeper->setToSweeper();

	//Connecting kissat to Kissat
	sweeper->sweepSetExportCallbacks();

	//Connecting kissat directly to SweepJob
    shweep_set_search_work_callback(sweeper->solver, this, cb_search_work_in_tree);
	shweep_set_SweepJob_eq_import_callback(sweeper->solver, this, cb_import_eq);
	shweep_set_SweepJob_unit_import_callback(sweeper->solver, this, cb_import_unit);

	if (_is_root) {
		//we want to read out the final formula at the root node for convenience, so we provide this callback only to root-node solvers in the first place
		sweeper->sweepSetReportCallback();
		sweeper->setRepresentativeLocalId(_representative_localId);
	}

    //Basic configuration
	// int quiet = _params.sweepSolverVerbosity()==0 ? 1 : 0;
    sweeper->set_option("quiet", _params.sweepSolverQuiet());  //suppress any standard kissat messages
    sweeper->set_option("verbose", 0);//the native kissat verbosity
    sweeper->set_option("log", 0);    //potentially extensive logging
    sweeper->set_option("check", 0);  // do not check model or derived clauses, because we import anyways units and equivalences without proof tracking
    sweeper->set_option("statistics", 1);  //print full statistics
    sweeper->set_option("profile", _params.satProfilingLevel.val); // do detailed profiling how much time we spent where
	sweeper->set_option("seed", 0);   //Sweeping should not contain any RNG part

	//Specific due to Mallob
	// printf("Mallob sweep Solver verbosity %i \n", _params.sweepSolverVerbosity.val);
	sweeper->set_option("mallob_custom_sweep_verbosity", _params.sweepSolverVerbosity.val); //Shweeper verbosity 0..4
	sweeper->set_option("mallob_is_shweeper", 1); //Make this Kissat solver a pure Distributed Sweeping Solver. Jumps directly to distributed sweeping and bypasses everything else
	sweeper->set_option("mallob_local_id", localId);
	sweeper->set_option("mallob_rank", _my_rank);
	sweeper->set_option("mallob_is_root", _is_root);
	sweeper->set_option("mallob_resweep_chance", _params.sweepResweepChance.val);
	sweeper->set_option("mallob_staggered_logs", 1); //set to 1 to have spatially separated logs, useful for verbose runs with 2-16 threads


	//Own options of Kissat
	sweeper->set_option("sweepcomplete", 1);      //deactivates checking for time limits during sweeping, so we dont get kicked out due to some limits
  	sweeper->set_option("sweepclauses", 1024);		//	1024, 0, INT_MAX,	"environment clauses")
  	sweeper->set_option("sweepmaxclauses", 32768);	//	32768,2, INT_MAX,	"maximum environment clauses")
  	sweeper->set_option("sweepdepth", 2);			//, 2,    0, INT_MAX,	"environment depth")
  	sweeper->set_option("sweepmaxdepth", 3);		//	3,    1, INT_MAX,	"maximum environment depth")
  	sweeper->set_option("sweepvars", 256);			//  256,  0, INT_MAX,	"environment variables")
  	sweeper->set_option("sweepmaxvars", 8192);		//	8192, 2, INT_MAX,	"maximum environment variables")
  	sweeper->set_option("sweepfliprounds", 2);		//	1,    0, INT_MAX,	"flipping rounds")
  	sweeper->set_option("sweeprand", 0);			//  0,    0,    1,		"randomize sweeping environment")

	//Specific for clean sweep run
	sweeper->set_option("preprocess", 0); //skip other preprocessing stuff after shweep finished
	// shweeper->set_option("probe", 1);   //there is some cleanup-probing at the end of the sweeping. keep it? (apparently the probe option is used nowhere anyways)
	sweeper->set_option("substitute", 1); //apply equivalence substitutions after sweeping (kissat default 1, but keep here explicitly to remember it)
	sweeper->set_option("substituterounds", 2); //default is 2, and changing that has currently no effect, virtually all substitutions happen in round 1, and already in round 2 zero or single substitutions are found, and it exits there.



	// shweeper->set_option("substituteeffort", 1000); //changes here dont seem to have much effect, basically all substituting already happens in the first round...
	// shweeper->set_option("substituterounds", 10);
	sweeper->set_option("luckyearly", 0); //skip
	sweeper->set_option("luckylate", 0);  //skip
	sweeper->interruptionInitialized = true;
	return sweeper;
}

void SweepJob::printSweepStats(KissatPtr sweeper, bool full) {
	assert(_is_root);
	assert(sweeper->getLocalId() == _representative_localId);

	auto stats = sweeper->fetchSweepStats();
	//As "vars" we are only interested in variables that are active (not fixed) at the start of Sweep. The "VAR" counter is much larger, but most of these variables are often already fixed.
	int vars_fixed_end  = stats.sweep_eqs + stats.units_new;
	int vars_remain_end = stats.vars_active_orig - vars_fixed_end;
	int clauses_removed = sweeper->_setup.numOriginalClauses - stats.clauses_end;

	double vars_fixed_percent = 100*vars_fixed_end/(double)stats.vars_active_orig;
	double vars_remain_percent = 100*vars_remain_end/(double)stats.vars_active_orig;
	double clauses_removed_percent = 100*clauses_removed/(double)sweeper->_setup.numOriginalClauses;

	LOG(			  V2_INFO, "SWEEP solver [%i](%i) reports statistics in dedicated .sweep file \n", _my_rank, sweeper->getLocalId());
	LOGGER(_reslogger,V2_INFO, "\n");
	LOGGER(_reslogger,V2_INFO, "Reported by [%i](%i) \n", _my_rank, sweeper->getLocalId());
	LOGGER(_reslogger,V2_INFO, "SWEEP_ROUND					%i / %i \n", _root_sweep_round, _params.sweepRounds());
	LOGGER(_reslogger,V2_INFO, "SWEEP_TIME					%f seconds \n", Timer::elapsedSeconds() - _start_sweep_timestamp);
	LOGGER(_reslogger,V2_INFO, "SWEEP_IMPORT_EQS_USEFUL		%i / %i \n", stats.eqs_useful, stats.eqs_seen); //representative for the reporting solver
	LOGGER(_reslogger,V2_INFO, "SWEEP_IMPORT_UNITS_USEFUL	%i / %i \n", stats.units_useful, stats.units_seen);
	LOGGER(_reslogger,V2_INFO, "SWEEP_EQUIVALENCES	%i\n", stats.sweep_eqs);
	LOGGER(_reslogger,V2_INFO, "SWEEP_UNITS_NEW		%i\n", stats.units_new);
	LOGGER(_reslogger,V2_INFO, "SWEEP_VARS_FIXED_N	%i / %i (%.3f %)\n", vars_fixed_end, stats.vars_active_orig, vars_fixed_percent);

	if (full) {
		LOGGER(_reslogger,V2_INFO, "SWEEP_PRIORITY       %.3f\n", _params.preprocessSweepPriority.val);
		LOGGER(_reslogger,V2_INFO, "SWEEP_PROCESSES      %i\n", getVolume());
		LOGGER(_reslogger,V2_INFO, "SWEEP_THREADS_PER_P  %i\n", _nThreads);
		LOGGER(_reslogger,V2_INFO, "SWEEP_SHARING_PERIOD %i ms \n", _params.sweepSharingPeriod_ms.val);
		LOGGER(_reslogger,V2_INFO, "SWEEP_VARS_ORIG      %i\n", sweeper->_setup.numVars);
		LOGGER(_reslogger,V2_INFO, "SWEEP_VARS_END       %i\n", stats.vars_end);
		LOGGER(_reslogger,V2_INFO, "SWEEP_ACTIVE_ORIG    %i\n", stats.vars_active_orig);
		LOGGER(_reslogger,V2_INFO, "SWEEP_ACTIVE_END     %i\n", vars_remain_end);
		LOGGER(_reslogger,V2_INFO, "SWEEP_CLAUSES_ORIG   %i\n", sweeper->_setup.numOriginalClauses);
		LOGGER(_reslogger,V2_INFO, "SWEEP_CLAUSES_END    %i\n", stats.clauses_end);
		LOGGER(_reslogger,V2_INFO, "SWEEP_UNITS_ORIG     %i\n", stats.units_orig);
		LOGGER(_reslogger,V2_INFO, "SWEEP_UNITS_END      %i\n", stats.units_end);
		LOGGER(_reslogger,V2_INFO, "SWEEP_ELIMINATED     %i\n", stats.eliminated);
		LOGGER(_reslogger,V2_INFO, "SWEEP_UNITS_SWEEP    %i\n", stats.sweep_units);
		LOGGER(_reslogger,V2_INFO, "SWEEP_CLAUSES_REMOVED_N		%i \n", clauses_removed);
		LOGGER(_reslogger,V2_INFO, "SWEEP_CLAUSES_REMOVED_PRCNT	%.6f \n", clauses_removed_percent);
		LOGGER(_reslogger,V2_INFO, "SWEEP_VARS_REMAIN_N			%i / %i (%.6f %)\n", vars_remain_end, stats.vars_active_orig, vars_remain_percent);
		LOGGER(_reslogger,V2_INFO, "SWEEP_VARS_FIXED_PRCNT		%.6f \n", vars_fixed_percent);
	}

	if (full) {
		static const int DURATION_WARN_FACTOR=2;
		float latency_sum=0, latency_avg=0, period_sum=0, period_avg=0;
		std::vector<float> latencies;
		for (int i=0; i < _time_start_bcast.size() && i < _time_receive_allred.size(); i++) {
			latencies.push_back(_time_receive_allred[i]-_time_start_bcast[i]);
			latency_sum += latencies.back();
		}
		if (!latencies.empty()) {
			latency_avg = latency_sum/latencies.size();
		}
		if (_time_start_bcast.size()>1) {
			period_sum = _time_start_bcast.back() - _time_start_bcast.front();
			period_avg = period_sum / (_time_start_bcast.size()-1);
		}
		LOGGER(_reslogger,V2_INFO, "SWEEP_SHARING_LATENCY     %.1f ms (average) \n",latency_avg*1000);
		LOGGER(_reslogger,V2_INFO, "SWEEP_SHARING_PERIOD_REAL %.1f ms (average) \n",period_avg*1000);


		if (_time_start_bcast.size()>1) {
			for (int i=0; i < _time_start_bcast.size()-1; i++) {
				float period = _time_start_bcast[i+1] - _time_start_bcast[i];
				if (period > DURATION_WARN_FACTOR*period_avg) {
					LOGGER(_reslogger,V2_INFO, "[WARN] SWEEP_SHARING_PERIOD_REAL %.1f ms   (in share round %i) is much larger than average \n", period, i);
				}
			}
		}

		for (int i=0; i< latencies.size(); i++) {
			if (latencies[i] > DURATION_WARN_FACTOR*latency_avg) {
				LOGGER(_reslogger,V2_INFO, "[WARN] SWEEP_SHARING_LATENCY %.1f ms     (between share rounds %i,%i) is much larger than average \n", latencies[i], i, i+1);
			}
		}

		for (int i=0; i<15 && i<_internal_result.getSolutionSize(); i++) {
			LOGGER(_reslogger,V3_VERB, "RESULT Sweep Formula[%i] = %i \n", i, _internal_result.getSolution(i));
		}
	}


	// LOG(V2_INFO, "SWEEP JOB [%i](%i) completed readStats \n", _my_rank, sweeper->getLocalId());
}


void SweepJob::printIdleFraction() {
	int idles = 0;
	int active = 0;
	std::ostringstream oss;
	for (auto sweeper : _sweepers) {
		if (sweeper) {
			active++;
			if (sweeper->sweeper_is_idle) {
				idles++;
				oss << "(" << sweeper->getLocalId() << ") ";
			}
		}
	}
	if (active>0)
		LOG(V3_VERB, "SWEEP IDLE  %i/%i : %s \n", idles, active, oss.str().c_str());
}

void SweepJob::printResweeps() {
	std::ostringstream oss;
	int worksweeps = 0;
	int resweeps_in = 0;
	int resweeps_out = 0;
	for (int i=0; i<_nThreads; i++) {
		oss << " (id=" << i
		<<" ws=" << _worksweeps[i]
		<<" rsi=" << _resweeps_in[i]
		<<" rso=" << _resweeps_out[i]
		<<") ";
		worksweeps += _worksweeps[i];
		resweeps_in += _resweeps_in[i];
		resweeps_out += _resweeps_out[i];
	}
	// LOG(V3_VERB, "SWEEP WORKSWEEPS,RESWEEPS: %s \n", _my_rank, oss.str().c_str()); //information for each individual thread
	LOG(V2_INFO, "[%i] SWEEP_WORKSWEEPS   %i \n", _my_rank, worksweeps);
	LOG(V2_INFO, "[%i] SWEEP_RESWEEPS_ALL %i \n", _my_rank, resweeps_in + resweeps_out);
	LOG(V3_VERB, "[%i] SWEEP_RESWEEPS_IN  %i \n", _my_rank, resweeps_in);
	LOG(V3_VERB, "[%i] SWEEP_RESWEEPS_OUT %i \n", _my_rank, resweeps_out);
}

void SweepJob::sendMPIWorkstealRequests() {
	//Worksteal requests need to be execute by the MPI main thread, as it can be problematic if the kissat-threads issue MPI messages in the callback on their own
	//Thus the solver-threads only write a request in shared memory, which is then picked up here by the main MPI thread
	for (auto &request : _worksteal_requests) {
		if (!request.sent) {
			request.sent = true;
			JobMessage msg = getMessageTemplate();
			msg.tag = TAG_SEARCHING_WORK;
			//Need to add these two fields because we are doing arbitrary point-to-point communication
			msg.treeIndexOfDestination = request.targetIndex;
			msg.contextIdOfDestination = getJobComm().getContextIdOrZero(request.targetIndex);

			assert(msg.contextIdOfDestination != 0 || log_return_false("SWEEP ERROR: contextIdOfDestination==0 in workstealing request! Source rank=%i, targetRank %i \n", _my_rank, request.targetRank));
			msg.payload = {request.localId};
			// LOG(V2_INFO, "Rank %i asks rank %i for work\n", _my_rank, recv_rank, n);
			// LOG(V2_INFO, "  with destionation ctx_id %i \n", msg.contextIdOfDestination);
			LOG(V3_VERB, "SWEEP MSG [%i](%i) ---?---> [%i] \n", _my_rank, request.localId, request.targetRank);
			getJobTree().send(request.targetRank, MSG_SEND_APPLICATION_MESSAGE, msg);
		}
	}
}


void SweepJob::checkForNewImportRound(KissatPtr sweeper) {
	int publish_round = _sharing_import_round.load(std::memory_order_relaxed);
	if (publish_round != sweeper->sweep_import_round) [[unlikely]] {
		//there is new data from a new sharing round
		publish_round = _sharing_import_round.load(std::memory_order_acquire);
		assert(sweeper->sweep_import_round <= publish_round);
		sweeper->sweep_import_round  = publish_round;
		if (sweeper->sweep_EQS_index != sweeper->sweep_EQS_size)
			LOG(V1_WARN, "WARN SWEEP: Solver [%i](%i) couldn't finish reading previous equivalence imports! now skipping remaining  %i/%i \n", _my_rank, sweeper->getLocalId(), sweeper->sweep_EQS_index.load(), sweeper->sweep_EQS_size.load());
		if (sweeper->sweep_UNITS_index != sweeper->sweep_UNITS_size)
			LOG(V1_WARN, "WARN SWEEP: Solver [%i](%i) couldn't finish reading previous unit imports! now skipping remaining  %i/%i \n", _my_rank, sweeper->getLocalId(), sweeper->sweep_UNITS_index.load(), sweeper->sweep_UNITS_size.load());
		//tell the solver where in the fixed array the data starts and where it ends
		sweeper->sweep_EQS_index   = 0;
		sweeper->sweep_UNITS_index = 0;
		sweeper->sweep_EQS_size    = _EQS_import_size.load(std::memory_order_relaxed);
		sweeper->sweep_UNITS_size  = _UNITS_import_size.load(std::memory_order_relaxed);
		// LOG(V2_INFO, "Solver [%i](%i) updates to new round %i with eq_size %i, unit_size %i \n", _my_rank, sweeper->getLocalId(), publish_round, sweeper->sweep_EQS_size.load(), sweeper->sweep_UNITS_size.load());
	}
}

void SweepJob::cbImportEq(int *ilit1, int *ilit2, int localId) {

	KissatPtr sweeper = _sweepers[localId];
	checkForNewImportRound(sweeper);

	// LOG(V2_INFO, "solver [%i](%i) eq callback sees index %i and size %i \n",_my_rank, localId, sweeper->sweep_curr_EQS_index.load(), sweeper->sweep_curr_EQS_size.load());
	if (sweeper->sweep_EQS_index == sweeper->sweep_EQS_size) {
		//leave *ilit's untouched
		return;
	}

	assert(sweeper->sweep_EQS_index < sweeper->sweep_EQS_size	|| log_return_false("SWEEP ERROR: in Equivalence Import: curr index %i is larger than expected size %i\n", sweeper->sweep_EQS_index.load(), sweeper->sweep_EQS_size.load()));
	int idx = sweeper->sweep_EQS_index.load();
	*ilit1 = _EQS_to_import[idx];
	*ilit2 = _EQS_to_import[idx+1];
	sweeper->sweep_EQS_index +=2;
	assert(*ilit1 !=0 || *ilit2 !=0								|| log_return_false("SWEEP ERROR: in cbImportEq: sending invalid empty *ilit1=%i, *ilit2=0 to the solvers\n", *ilit1, *ilit2));
	assert(*ilit1 < *ilit2										|| log_return_false("SWEEP ERROR: in cbImportEq: *ilit1 %i is larger than %i *ilit2, but they should be sorted\n", *ilit1, *ilit2));
	assert(sweeper->sweep_EQS_index <= sweeper->sweep_EQS_size	|| log_return_false("SWEEP ERROR: in Equivalence Import: index %i is now beyond size %i \n", sweeper->sweep_EQS_index.load(), sweeper->sweep_EQS_size.load()));
	//now returning to kissat solver
}

int SweepJob::cbCustomQuery(int query) {
	if (query==QUERY_SWEEP_ROUND) {
		return _root_sweep_round;
	}

	return 0;
}

void SweepJob::cbImportUnit(int *ilit, int localId) {
	KissatPtr sweeper = _sweepers[localId];
	checkForNewImportRound(sweeper);
	if (sweeper->sweep_UNITS_index == sweeper->sweep_UNITS_size) {
		// leave *ilit untouched
		return;
	}
	assert(sweeper->sweep_UNITS_index < sweeper->sweep_UNITS_size || log_return_false("SWEEP ERROR: in Unit Import: curr index %i is larger than expected size %i\n", sweeper->sweep_UNITS_index.load(), sweeper->sweep_UNITS_size.load()));
	int idx = sweeper->sweep_UNITS_index.load();
	*ilit = _UNITS_to_import[idx];
	// LOG(V2_INFO, "Sending Unit %i (index %i) to solver (%i)\n", *ilit, idx, localId);
	sweeper->sweep_UNITS_index++;
	// assert(*ilit1 < *ilit2 || log_return_false("SWEEP ERROR: in cbImportEq: *ilit1 %i is larger than %i *ilit2, but they should be sorted\n", *ilit1, *ilit2));
	assert(sweeper->sweep_UNITS_index <= sweeper->sweep_UNITS_size || log_return_false("SWEEP ERROR: in Unit Import: index %i is now beyond size %i \n", sweeper->sweep_UNITS_index.load(), sweeper->sweep_UNITS_size.load()));

	//now returning to kissat solver
}

void SweepJob::provideInitialWork(KissatPtr sweeper) {
	assert(!_root_provided_initial_work);
	assert(sweeper->work_received_from_steal.empty());
	assert(_is_root);
	assert(sweeper->getLocalId()==_representative_localId);
	// assert(sweeper->sweeper_is_idle);


	_root_provided_initial_work = true;
	sweeper->sweeper_is_idle = false; //already set non-idle here to prevent case where solver is already initialized, non-idle, but still has no work cause its just being copied, and then a sharing operation starts right now, terminating everything wrongly early

	//We need to know how much space to allocate to store each variable "idx" at the array position work[idx].
	//i.e. we need to know max(idx).
	//We assume that the maximum variable index corresponds to the total number of variables
	//i.e. that there are no holes in kissats internal numbering. This is an assumption that standard Kissat makes all the time, so we also do it here

	const unsigned VARS = shweep_get_num_vars(sweeper->solver); //this value can be different from numVars here in C++ !! Because kissat might havel aready propagated some units, etc.
	sweeper->work_received_from_steal = std::vector<int>(VARS);
	//the initial work is all variables
	for (int idx = 0; idx < VARS; idx++) {
		sweeper->work_received_from_steal[idx] = idx;
	}
	LOG(V2_INFO, "SWEEP WORK PROVIDED: All work --------------%u---------------->sweeper [%i](%i)\n", VARS, _my_rank, sweeper->getLocalId());
}


void SweepJob::cbSearchWorkInTree(unsigned **work, int *work_size, int localId) {
	KissatPtr sweeper = _sweepers[localId]; //array access safe (we know the sweeper exists) because this callback is called by this sweeper itself
	sweeper->work_received_from_steal = {};
	//setting this sweeper to idle is done more fine grained in the following lines, because it caused a race condition here for the very first solver that was marked idle while it was receiving the initial work

	//loop until we find work or the whole sweeping is terminated
	while (true) {
		LOG(V5_DEBG, "Sweeper [%i](%i) in steal loop\n", _my_rank, localId);

		if (_terminate_all) {
			//this is the signal for the solver to terminate itself, by sending it a work array of size 0
			sweeper->work_received_from_steal = {};
			sweeper->sweeper_is_idle = true;
			LOG(V3_VERB, "Sweeper [%i](%i) exit steal loop\n", _my_rank, localId);
			break;
		}
		 /*
		  * We serve the initial work exclusively to solver 0 at the root node. This hardcoded target prevents any concurrency/communication problems at this stage.
		  */
		if (_is_root && ! _root_provided_initial_work && localId==_representative_localId) {
			provideInitialWork(sweeper);
			break;
		}

		//Try to steal locally from shared memory
		 /*
		  * Going for some direct global steals doesnt do much here, because the big rally happens anyways the moment all are depleted locally
		  * At that point they anyways schedule a global sweep, and some sweepers stealing globally before cant stop that
		  */

		LOG(V5_DEBG, "SWEEP WORK [%i](%i) steal loop --> local steal \n", _my_rank, localId);
		sweeper->sweeper_is_idle = true;
		auto stolen_work = stealWorkFromAnyLocalSolver(_my_rank, localId);

		//Successful local steal
		if ( ! stolen_work.empty()) {
			//store steal data persistently in C++, such that C can keep operating on that memory segment
			sweeper->work_received_from_steal = std::move(stolen_work);
			LOG(V3_VERB, "SWEEP WORK [%i](%i) <==%i==== [%i]  (local) \n", localId, _my_rank, sweeper->work_received_from_steal.size(), _my_rank);
			break;
		}
		LOG(V5_DEBG, "SWEEP WORK [%i](%i) steal loop <-- local steal failed \n", _my_rank, localId);
		int my_comm_rank = getJobComm().getWorldRankOrMinusOne(_my_index);
		if (my_comm_rank == -1) {
			LOG(V3_VERB, "SWEEP SKIP Delaying global steal request, my own rank %i (index %i) is not yet in JobComm \n", _my_rank, _my_index);
			usleep(500);
			continue;
		}
		//Unsuccessful steal locally. Go global via MPI message
		assert(getVolume()>=1 || log_return_false("SWEEP ERROR [%i](%i) in workstealing: getVolume()==%i, i.e. no volume available to steal from\n", _my_rank, localId, getVolume()));
		if (getVolume()==1) {
			LOG(V5_DEBG, "SWEEP WORK SKIP [%i](%i) steal loop --> we are the only MPI rank, no global steal \n", _my_rank, localId);
			usleep(500);
			continue;
		}
		int targetIndex = _rng.randomInRange(0,getVolume());
        int targetRank = getJobComm().getWorldRankOrMinusOne(targetIndex);
        if (targetRank == -1) {
        	//target rank not yet in JobTree, might need some more milliseconds to update, try again
			LOG(V3_VERB, "SWEEP WORK SKIP targetIndex %i, targetRank %i not yet in JobComm\n", targetIndex, targetRank);
			usleep(500);
        	continue;
        }
		if (targetRank == _my_rank) {
			// not stealing from ourselves, try again
			continue;
		}
		if (getJobComm().getContextIdOrZero(targetIndex)==0) {
			LOG(V3_VERB, "SWEEP WORK SKIP Context ID of target is missing. getVolume()=%i, rndTargetIndex=%i, rndTargetRank=%i, myIndex=%i, myRank=%i \n", getVolume(), targetIndex, targetRank, _my_index, _my_rank);
			//target is not yet listed in address list. Might happen for a short period just after it is spawned
			usleep(500);
			continue;
		}
		//Request will be handled by the MPI main thread, which will send an MPI message on our behalf
		//because here we are in code executed by the kissat thread, which can cause problems for sending MPI messages
		LOG(V5_DEBG, "SWEEP WORK [%i](%i) steal loop --> global steal to [%i] \n", _my_rank, localId, targetRank);
		WorkstealRequest request;
		request.localId = localId;
		request.targetIndex = targetIndex;
		request.targetRank = targetRank;
		_worksteal_requests[localId] = request;
		//Wait here until we get back an MPI message
		unsigned reps=0;
		while( ! _worksteal_requests[localId].got_steal_response) {
			usleep(100);
			reps++;
			if (reps%32==0) {
				LOG(V5_DEBG, "SWEEP WORK [%i](%i) steal loop: waits for MPI response\n", _my_rank, localId);
			}
			if (_terminate_all) {
				LOG(V3_VERB, "SWEEP WORK [%i](%i) exit wait loop\n", _my_rank, localId);
				break;
			}
		}
		//Successful steal if size > 0
		if ( ! _worksteal_requests[localId].stolen_work.empty()) {
			sweeper->work_received_from_steal = std::move(_worksteal_requests[localId].stolen_work);
			LOG(V4_VVER, "SWEEP MPI WORK [%i](%i) <==%i=== [%i] \n",  _my_rank, localId, sweeper->work_received_from_steal.size(), targetRank);
			break;
		}
		LOG(V5_DEBG, "SWEEP WORK [%i](%i) steal loop <-- global steal to [%i] failed \n", _my_rank, localId, targetRank);
		//Unsuccessful global steal, try again
	}
	//Found work (if work_size>0) or got signal for termination (work_size==0).
	//we control the memory on C++/Mallob Level, and only tell the kissat solver where it can find the work (or where it can read the zero)
	//the solver will also write on this array, but only within the allocated bounds provided by C++/Mallob
	*work = reinterpret_cast<unsigned int*>(sweeper->work_received_from_steal.data());
	*work_size = sweeper->work_received_from_steal.size();
	assert(*work_size>=0);
	if (*work_size>0) {
		sweeper->sweeper_is_idle = false; //we keep solver marked as "idle" once the whole solving is terminated, otherwise race-conditions can occur where suddenly the solver is treated as active again
	}
	//The thread now returns to the kissat solver
}


void SweepJob::initiateNewSharingRound() {
	if (!_bcast) {
		LOG(V1_WARN, "SWEEP Warn : SHARE BCAST root couldn't initiate sharing round, _bcast is Null\n");
		return;
	}


	if (Timer::elapsedSeconds() < _last_sharing_start_timestamp + _params.sweepSharingPeriod_ms.val/1000.0) {
		//not yet time for next sharing round
		return;
	}

// #if STARTUP_TYPE == 2
	if (!_started_synchronized_solving) {
		LOG(V3_VERB, "SWEEP SHARE BCAST: Delaying first sharing operation, not all solvers online yet (%i/%i) \n", _running_sweepers_count.load(), _nThreads);
		return;
	}
// #endif

	//make sure that only one sharing operation is going on at a time
	if (_bcast->hasReceivedBroadcast()) {
		LOG(V3_VERB, "SWEEP SHARE BCAST: New sharing round delayed, old round is not completed yet\n");
		return;
	}
	//Broadcast a ping to all workers to initiate an AllReduce
	//The broadcast includes all workers currently reachable by the root-node and informs them about their parent and potential children
	//It then causes the leaf nodes to call the callback, initiating the AllReduce
	_last_sharing_start_timestamp = Timer::elapsedSeconds();
	_time_start_bcast.push_back(_last_sharing_start_timestamp);
	LOG(V3_VERB, "SWEEP SHARE BCAST Initiating Sharing via Ping\n");
	JobMessage msg = getMessageTemplate();
	msg.tag = _bcast->getMessageTag();
	msg.payload = {};
	_bcast->broadcast(std::move(msg));
}

void SweepJob::appendMetadataToReductionElement(std::vector<int> &contrib, int is_idle, int unit_size, int eq_size) {
	contrib.insert(contrib.end(), NUM_METADATA_FIELDS, 0); //Make space for the upcoming metadata
	int size = contrib.size();
	int n=0;
	n++; contrib[size - METADATA_POS_TERMINATE_FLAG]  = 0;  //dummy, will be set by root transformation
	n++; contrib[size - METADATA_POS_ROUND_FLAG]      = 0;  //dummy, will be set by root transformation
	n++; contrib[size - METADATA_POS_IDLE_FLAG]       = is_idle;
	n++; contrib[size - METADATA_POS_UNIT_SIZE]       = unit_size;
	n++; contrib[size - METADATA_POS_EQ_SIZE]         = eq_size;
	assert(n==NUM_METADATA_FIELDS || log_return_false("SWEEP ERROR: Added metadata count (%i) doesnt match expected number (%i) \n", n, NUM_METADATA_FIELDS));
}

void SweepJob::cbContributeToAllReduce() {
	assert(_bcast);
	assert(_bcast->hasResult());

	LOG(V4_VVER, "SWEEP SHARE BCAST Callback to AllReduce\n");
	auto snapshot = _bcast->getJobTreeSnapshot();

	if (_terminate_all) {
		LOG(V4_VVER, "SWEEP SHARE BCAST skip reduction, status is already _terminate_all\n");
		return;
	}

	if (! _is_root) {
		LOG(V4_VVER, "SWEEP SHARE [%i] RESET non-root BCAST\n", _my_rank);
		_bcast.reset(new JobTreeBroadcast(getId(), getJobTree().getSnapshot(),
			[this]() {cbContributeToAllReduce();}, TAG_BCAST_INIT));
		//root is reset only after the whole reduction result is broadcasted, to prevent starting a new one while the old one is still running
		//(might change this to add overlapping broadcasts later for faster turnovers, but for now keep only one for cleaner debugging)
	}


	JobMessage baseMsg = getMessageTemplate();
	baseMsg.tag = TAG_ALLRED;
	LOG(V4_VVER, "SWEEP SHARE [%i] RESET AllReduction, to prepare contributing local data. \n", _my_rank);
	_red.reset(new JobTreeAllReduction(snapshot, baseMsg, std::vector<int>(), aggregateEqUnitContributions));
	if (_is_root)
		_red->setInplaceTransformationOfElementAtRoot(_inplace_rootTransform);


	//Bring individual data per thread in the sharing element format: [Equivalences, Units, eq_size, unit_size, all_idle]
	std::list<std::vector<int>> contribs;
	int id=-1; //for debugging
	for (auto &sweeper : _sweepers) {
		id++;
		if (!sweeper) {
			LOG(V4_VVER, "SWEEP SHARE [%i](%i) not yet initialized, skipped in contribution aggregation \n", _my_rank, id);
			continue;
		}

		//Mutex, because the solvers are asynchronously pushing all the time new eqs&units into these vectors (including reallocations after push_back), make sure that doesn't happen while we copy/move
		std::vector<int> eqs, units;
		{
			std::lock_guard<std::mutex> lock(sweeper->sweep_export_mutex);
			eqs = std::move(sweeper->eqs_to_share);
			units = std::move(sweeper->units_to_share);
			//by moving we also clear their current position, i.e. prevents from sharing them twice
		}

		int eq_size = eqs.size();
		int unit_size = units.size();
		assert(eq_size%2==0 || log_return_false("ERROR in AGGR: Non-even number %i of equivalence literals, should always come in pairs", eq_size)); //equivalences come always in pairs
		//we need to glue together equivalences and units. can use move on the equivalences to save a copying of them, and only need to copy the units
		//moved logging before the actions, because this code triggered std::bad_alloc once, might give some more info next time
		LOG(V4_VVER, "SWEEP SHARE REDUCE (%i): %i eq_size, %i units, %i idle \n", sweeper->getLocalId(), eq_size, unit_size, sweeper->sweeper_is_idle);
		std::vector<int> contrib = std::move(eqs);
		contrib.insert(contrib.end(), units.begin(), units.end());

		appendMetadataToReductionElement(contrib, sweeper->sweeper_is_idle, unit_size, eq_size);

		contribs.push_back(contrib);
	}

	auto aggregation_element = aggregateEqUnitContributions(contribs);

	LOG(V4_VVER, "SWEEP SHARE REDUCE [%i] ~~~%i~~~(+%i)~~> to sharing \n", _my_rank, aggregation_element.size()-NUM_METADATA_FIELDS, NUM_METADATA_FIELDS);

	if (_terminate_all) {
		LOG(V4_VVER, "SWEEP SHARE BCAST skip contribution, seen already _terminate_all\n");
		return;
	}

	_red->contribute(std::move(aggregation_element));

}

void SweepJob::advanceAllReduction() {
	if (!_red) return;
	if (!_started_synchronized_solving) return;
	LOG(V4_VVER, "SWEEP SHARE REDUCE ADVANCE [%i]\n", _my_rank);
	_red->advance();
	if (!_red->hasResult()) return;

	// LOG(V3_VERB, "[sweep] all-reduction complete\n");
	_time_receive_allred.push_back(Timer::elapsedSeconds());

	//todo: Filter out duplicates during aggregating? Is it worth the effort?

	//Extract shared data
	auto data = _red->extractResult();
	const int terminate   = data[data.size()-METADATA_POS_TERMINATE_FLAG];
	const int sweep_round = data[data.size()-METADATA_POS_ROUND_FLAG];
	const int all_idle    = data[data.size()-METADATA_POS_IDLE_FLAG];
	const int unit_size   = data[data.size()-METADATA_POS_UNIT_SIZE];
	const int eq_size     = data[data.size()-METADATA_POS_EQ_SIZE];
	assert(eq_size%2==0 || log_return_false("ERROR: Import Equality size not even, but %i\n", eq_size));

	for (int i=0; i<eq_size; i++) {
		_EQS_to_import[i] = data[i];
	}

	for (int i=eq_size; i<data.size()-NUM_METADATA_FIELDS; i++) {
		_UNITS_to_import[i-eq_size] = data[i];
	}

	_EQS_import_size.store(eq_size, std::memory_order_relaxed);
	_UNITS_import_size.store(unit_size, std::memory_order_relaxed);
	_sharing_import_round.store(_sharing_import_round+1, std::memory_order_release);

	for (auto &sweeper : _sweepers) {
		if (sweeper) {
			shweep_set_sweep_round(sweeper->solver, sweep_round);
		}
	}

	LOG(V2_INFO, "SWEEP sharing import round %i got: %i EQS, %i UNITS\n", _sharing_import_round.load(), eq_size/2, unit_size);

	//Now we can prepare a new sharing operation starting from the root node, because the old sharing (broadcast + allreduce) is now finished
	if (_is_root) {
		LOG(V4_VVER, "SWEEP SHARE [%i] RESET root BCAST\n", _my_rank);
		_bcast.reset(new JobTreeBroadcast(getId(), getJobTree().getSnapshot(),
			[this]() {cbContributeToAllReduce();}, TAG_BCAST_INIT));
	}

	LOG(V4_VVER, "SWEEP SHARE [%i] RESET REDUCE\n", _my_rank);
	_red.reset();


	if (all_idle) {
		LOG(V1_WARN, "[%i] got sharing info: sweep round finished \n", _my_rank);
	}

	if (terminate) {
		_terminate_all = true;
		LOG(V1_WARN, "# \n # \n # --- [%i] got terminate flag, TERMINATING SWEEP JOB ---\n # \n", _my_rank);
	}

}




std::vector<int> SweepJob::aggregateEqUnitContributions(std::list<std::vector<int>> &contribs) {
	//Each contribution has the format [Equivalences,Units, eq_size,unit_size,all_idle].
	//									-----data---------  ------metadata------------

	// std::ostringstream oss_tot;
	// oss_tot << "Contrib sizes: ";
	// for (int id : rand_permutation) {
		// oss << id << ' ';
	// }
	// LOG(V4_VVER, "%s \n", oss.str().c_str());

	//sanity check whether each contribution contains coherent size information about itself
	for (const auto& contrib : contribs) {
		int claimed_eq_size = contrib[contrib.size()- METADATA_POS_EQ_SIZE];
		int claimed_unit_size = contrib[contrib.size()- METADATA_POS_UNIT_SIZE];
		int claimed_total_size = claimed_eq_size + claimed_unit_size + NUM_METADATA_FIELDS;
		assert(contrib.size() == claimed_total_size ||
			log_return_false("ERROR in AllReduce, Bad Element Format: Claims total size %i != %i actual contrib.size() (claims: eq_size %i,  units size %i, metadata %i)", claimed_total_size, contrib.size(), claimed_eq_size, claimed_unit_size, NUM_METADATA_FIELDS)
			);
	}


	LOG(V4_VVER, "SWEEP AGGR %i contributions \n", contribs.size());
	size_t total_aggregated_size = NUM_METADATA_FIELDS; //the new element will contain the metadata once at the end
	int i=0;
    for (const auto& contrib : contribs) {
	    total_aggregated_size += contrib.size()-NUM_METADATA_FIELDS; //we will copy everything but the metadata from each contribution
		LOG(V4_VVER, "SWEEP AGGR Element %i: contrib.size() %i, w/o metadata %i, curr summed size %i \n", i, contrib.size(), contrib.size()-NUM_METADATA_FIELDS, total_aggregated_size);
		// oss_tot << "| " << i << ":" << contrib.size() << " "	;
		i++;
    	assert(contrib.size() >= NUM_METADATA_FIELDS || log_return_false("ERROR in Aggregating: contrib with too small size() == %i < %i SHARING_METADATA_FIELDS", contrib.size(), NUM_METADATA_FIELDS));
    }



	// LOG(V4_VVER, "%s \n", oss_tot.str().c_str());
	// std::ostringstream oss_e;
	// oss_e << "Eq sizes: ";
	i=0;


    std::vector<int> aggregated;
    aggregated.reserve(total_aggregated_size);
	//Fill equivalences
	size_t aggr_eq_size = 0;
    for (const auto &contrib : contribs) {
    	int eq_size = contrib[contrib.size()-METADATA_POS_EQ_SIZE];
    	aggr_eq_size += eq_size;
        aggregated.insert(aggregated.end(), contrib.begin(), contrib.begin()+eq_size);
		// oss_e << "| " << i << ":" << eq_size << " "	;
    	i++;
    }

	// LOG(V4_VVER, "%s \n", oss_e.str().c_str());
	// std::ostringstream oss_u;
	// oss_u << "Unit sizes: ";
	i=0;

	//Fill units
	size_t aggr_unit_size = 0;
    for (const auto &contrib : contribs) {
    	int eq_size = contrib[contrib.size()-METADATA_POS_EQ_SIZE];  //need to know where the eq ends, i.e. where the units start
    	int unit_size = contrib[contrib.size()-METADATA_POS_UNIT_SIZE];
		aggr_unit_size += unit_size;
        aggregated.insert(aggregated.end(), contrib.begin()+eq_size, contrib.end()-NUM_METADATA_FIELDS); //not copying the metadata at the end
    	// oss_u << "| " << i << ":" << unit_size << " "	;
    	i++;
    }

	// LOG(V4_VVER, "%s \n", oss_u.str().c_str());

	//See whether all solvers are idle
	bool all_idle = true;
    for (const auto &contrib : contribs) {
		bool idle = contrib[contrib.size()-METADATA_POS_IDLE_FLAG];
    	all_idle &= idle;
    }

	if (contribs.empty()) {
		all_idle = false; //edge-case: if not a single solver is initialized yet, we are waiting for them to come online, so they are not really idle
	}

	//Get round
	// int _sweep_round =

	appendMetadataToReductionElement(aggregated, all_idle, aggr_unit_size, aggr_eq_size);

	if (contribs.size()>1)
		LOG(V3_VERB, "SWEEP SHARE REDUCE aggr %i contribs: %i EQ, %i UNITS, %i ALL_IDLE\n", contribs.size(), aggr_eq_size/2, aggr_unit_size, all_idle);
	int individual_sum =  aggr_eq_size + aggr_unit_size + NUM_METADATA_FIELDS;
	assert(total_aggregated_size == individual_sum ||
		log_return_false("SWEEP ERROR: aggregated element assert failed: total_size %i != %i individual_sum (total_eq_size %i + total_unit_size %i + metadata %i) ",
			total_aggregated_size, individual_sum, aggr_eq_size, aggr_unit_size, NUM_METADATA_FIELDS));
    return aggregated;
}



std::vector<int> SweepJob::stealWorkFromAnyLocalSolver(int asking_rank, int asking_localId) { //Parameters are only used for verbose logging, don't influence function behaviour
	auto rand_permutation = getRandomIdPermutation();

	for (int localId : rand_permutation) {
		auto stolen_work = stealWorkFromSpecificLocalSolver(localId);
		if ( ! stolen_work.empty()) {
			LOG(V3_VERB, "SWEEP WORK [%i](%i) ====%i==> [%i](%i) \n",_my_rank, localId, stolen_work.size(), asking_rank, asking_localId);
			return stolen_work;
		}
	}
	//no work available at the local rank
	return {};
}

std::vector<int> SweepJob::stealWorkFromSpecificLocalSolver(int localId) {
	if (_terminate_all) //sweeping finished globally, nothing to steal anymore
		return {};
	if ( ! _sweepers[localId]) {
		// LOG(V3_VERB, "SWEEP STEAL stealing from [%i](%i), shweeper does not exist yet\n", _my_rank, localId);
		return {};
	}
	KissatPtr sweeper = _sweepers[localId];
	if ( ! sweeper->solver) {
		// LOG(V3_VERB, "SWEEP STEAL stealing from [%i](%i), shweeper->solver does not exist yet \n", _my_rank, localId);
		return {};
	}

	//We dont know yet how much there is to steal, so we ask for an upper bound
	//It can also be that the solver we want to steal from is not fully initialized yet
	//For that in the C code there are further guards against unfinished initialization, all returning 0 in that case
	// LOG(V3_VERB, "SWEEP STEAL [%i] getting max steal info from (%i) \n", _my_rank, localId);
	int max_steal_amount = shweep_get_max_steal_amount(sweeper->solver);
	if (max_steal_amount == 0)
		return {};

	// LOG(V2_INFO, "ß %i max_steal_amount\n", max_steal_amount);
	assert(max_steal_amount > 0 || log_return_false("SWEEP STEAL ERROR [%i](%i): negative max steal amount %i, maybe segfault into non-initialized kissat solver \n", _my_rank, localId, max_steal_amount));
	assert(max_steal_amount < 2*_numVars || log_return_false("SWEEP STEAL ERROR [%i](%i): too large max steal amount %i >= 2*NUM_VARS, maybe segfault into non-initialized kissat solver \n", _my_rank, localId, max_steal_amount));

	//There is something to steal
	//Allocate memory for the steal here in C++, and pass the array location to kissat such that it can fill it with the stolen work
	std::vector<int> stolen_work = std::vector<int>(max_steal_amount);

	// LOG(V3_VERB, "[%i] stealing from (%i), expecting max %i  \n", _my_rank, localId, max_steal_amount);
	int actually_stolen = shweep_steal_from_this_solver(sweeper->solver, reinterpret_cast<unsigned int*>(stolen_work.data()), max_steal_amount);
	// LOG(V3_VERB, "ß Steal request got %i actually stolen\n", actually_stolen);
	// LOG(V3_VERB, "SWEEP WORK actually stolen ---%i---> from [%i](%i)\n", localId, stolen_work.size());
	if (actually_stolen == 0)
		return {};
	//We sized he provided array to be maximally conservative,
	//Now we learned how much there way actually to steal, shrink the array to have .size() match with the stolen amount
	stolen_work.resize(actually_stolen);
	return stolen_work;
}

std::vector<int> SweepJob::getRandomIdPermutation() {
	auto permutation = _list_of_ids; //copy
	static thread_local std::mt19937 rng(std::random_device{}()); //created/seeded only once per thread, then just advancing rng calls
	std::shuffle(permutation.begin(), permutation.end(), rng);
	return permutation;
}


void SweepJob::loadFormula(KissatPtr sweeper) {
	const int* lits = getDescription().getFormulaPayload(0);
	const int payload_size = getDescription().getFormulaPayloadSize(0);
	// LOG(V2_INFO, "SWEEP Loading Formula, size %i \n", payload_size);
	for (int i = 0; i < payload_size ; i++) {
		sweeper->addLiteral(lits[i]);
	}
}

void SweepJob::gentlyTerminateSolvers() {
	LOG(V3_VERB, "SWEEP TERM #%i [%i] interrupting solvers\n", getId(), _my_rank);

	// while () {
		// LOG(V1_WARN, "Warn SWEEP JOB [%i]: delaying destructors until sweepers are all cleanly initialized (until now init %i/%i)\n",
			// _my_rank, _started_sweepers_count.load(), _nThreads);
		// usleep(500);
	// }
	//each sweeper checks constantly for the interruption signal (on the ms scale or faster), allow for gentle own exit
	while (_started_sweepers_count < _nThreads || _running_sweepers_count>0) {
		LOG(V3_VERB, "SWEEP TERM #%i [%i] still %i solvers running\n", getId(), _my_rank, _running_sweepers_count.load());
		int i=0;
		for (auto &sweeper : _sweepers) {
			if (sweeper) {
				sweeper->triggerSweepTerminate();
				LOG(V3_VERB, "SWEEP TERM #%i [%i] terminating solver (%i)\n", getId(), _my_rank, i);
			}
			i++;
		}
		usleep(500);
	}
	LOG(V3_VERB, "SWEEP TERM #%i [%i] no more solvers running\n", getId(), _my_rank);

	usleep(500);

	int i=0;
	LOG(V3_VERB, "SWEEP TERM #%i [%i] joining bg_workers \n",  getId(),_my_rank);
	for (auto &bg_worker : _bg_workers) {
		if (bg_worker->isRunning()) {
			LOG(V3_VERB, "SWEEP TERM #%i [%i] joining bg_worker (%i) \n",  getId(),_my_rank, i);
			bg_worker->stop();
			LOG(V3_VERB, "SWEEP TERM #%i [%i] joined  bg_worker    (%i) \n",  getId(),_my_rank, i);
		}
		i++;
	}
	LOG(V3_VERB, "SWEEP TERM #%i [%i] joined all bg_workers \n", getId(),_my_rank);
	LOG(V3_VERB, "SWEEP TERM #%i [%i] DONE \n", getId(),_my_rank);
}

SweepJob::~SweepJob() {
	LOG(V3_VERB, "SWEEP JOB DESTRUCTOR ENTERED \n");
	gentlyTerminateSolvers();
	LOG(V3_VERB, "SWEEP JOB DESTRUCTOR DONE\n");
}














