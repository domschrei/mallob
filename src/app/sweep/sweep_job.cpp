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
    : Job(params, setup, table),
	_reslogger(Logger::getMainInstance().copy("<RESULT>", ".sweep")),
	_warnlogger(Logger::getMainInstance().copy("<WARN>", ".warn"))
{
	assert(_params.jobCommUpdatePeriod() > 0 || log_return_false("[ERROR] For this application to work,"
            " you must explicitly enable job communicators with the -jcup option, e.g., -jcup=0.1\n"));

	LOG(V2_INFO, "## \n");
	LOG(V2_INFO, "New SweepJob MPI Process on rank [%i] with %i threads, ctx %i \n", getJobTree().getRank(), params.numThreadsPerProcess.val, getJobTree().getContextId());
	LOG(V2_INFO, "## \n");
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
	if (_params.sweepIterations.val==0) {
		LOG(V2_INFO,"Skip SWEEP JOB, as sweepIterations==0");
		return;
	}
	_started_appl_start = true;
	_my_rank = getJobTree().getRank();
	_my_index = getJobTree().getIndex();
	_my_ctx_id = getJobTree().getContextId();
	_is_root = getJobTree().isRoot();
	_nThreads = min( getNumThreads(), _params.numThreadsPerProcess.val);
	if (_nThreads < _params.numThreadsPerProcess.val) {
		LOG(V1_WARN,"SWEEP WARN cut down threads to %i \n", _nThreads);
	}
	const JobDescription& desc = getDescription();
	int numVars = desc.getAppConfiguration().fixedSizeEntryToInt("__NV");
	int numClauses = desc.getAppConfiguration().fixedSizeEntryToInt("__NC");

	LOG(V2_INFO,"SWEEP JOB SweepJob appl_start() STARTED: Rank %i, Index %i, ContextId %i, is root? %i, Parent-Rank %i, Parent-Index %i, threads=%d, NumVars %i, NumClauses %i\n",
		_my_rank, _my_index, getJobTree().getContextId(), _is_root, getJobTree().getParentNodeRank(), getJobTree().getParentIndex(), _nThreads, numVars, numClauses);
	// LOG(V2_INFO,"SWEEP JOB sweep-sharing-period: %i ms\n", _params.sweepSharingPeriod_ms.val);
	// LOG(V2_INFO, "New SweepJob rank %i working on %i vars in %i clauses \n", getJobTree().getRank(), );
    _metadata = getSerializedDescription(0)->data();
	_start_sweep_timestamp = Timer::elapsedSeconds();
	_root_last_sharing_start_timestamp = Timer::elapsedSeconds();

    _reslogger = Logger::getMainInstance().copy("<RESULT>", ".sweep");
    _warnlogger = Logger::getMainInstance().copy("<WARN>", ".warn");

	_worksteal_requests.resize(_nThreads);

	//do not send the initial placeholder worksteal requests
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

	std::ostringstream oss;
	for (int localId=0; localId < _nThreads; localId++) {
		_list_of_ids.push_back(localId);
		oss << "," << localId;
	}
	LOG(V2_INFO,"SWEEP LIST_OF_LOCAL_IDS: %s \n", oss.str().c_str());

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
	LOG(V2_INFO,"SWEEP Create solvers\n");
	for (int localId=0; localId < _nThreads; localId++) {
		createAndStartNewSweeper(localId);
	}

	//might as well already set some result metadata information now
	_internal_result.id = getId();
	_internal_result.revision = getRevision();

	LOG(V2_INFO, "SWEEP appl_start() FINISHED\n");
	// _finished_job_setup = true;
}



// Called periodically by the main thread to allow the worker to emit messages.
void SweepJob::appl_communicate() {
	LOG(V5_DEBG, "SWEEP appl_communicate() \n");

	if (_bcast && _is_root && !_terminate_all)
		_bcast->updateJobTree(getJobTree());

	advanceAllReduction(); //always advance
	TryWorkstealLocal();
	TryWorkstealMPI();
	rootInitiateNewSharingRound();

	checkSharingDelay();
	printIdleFraction();
	checkForUnsatResults();
	LOG(V5_DEBG, "SWEEP appl_communicate() done \n");
}


// React to an incoming message. (This becomes relevant only if you send custom messages)
void SweepJob::appl_communicate(int sourceRank, int mpiTag, JobMessage& msg) {
	// LOG(V2_INFO, "Shweep rank %i: received custom message from source %i, mpiTag %i, msg.tag %i \n", _my_rank, source, mpiTag, msg.tag);
	if (mpiTag != MSG_SEND_APPLICATION_MESSAGE) {
		LOG(V1_WARN, "SWEEP MSG WARN [%i] got unexpected message with mpiTag=%i, msg.tag=%i  (instead of MSG_SEND_APPLICATION_MESSAGE mpiTag == 30)\n", _my_rank, mpiTag, msg.tag);
	}
	if (msg.returnedToSender) {
		LOG(V1_WARN, "SWEEP MSG WARN [%i]: received unexpected returnedToSender message during Sweep Job Workstealing! source=%i mpiTag=%i, msg.tag=%i treeIdxOfSender=%i, treeIdxOfDestination=%i \n", _my_rank, sourceRank, mpiTag, msg.tag, msg.treeIndexOfSender, msg.treeIndexOfDestination);
		// assert(log_return_false("SWEEP MSG ERROR, got msg.returnToSender"));
	}
	else if (msg.tag == TAG_SEARCHING_WORK) {
		assert(msg.payload.size() == NUM_SEARCHING_WORK_FIELDS);
		int sourceLocalId = msg.payload[0];
		int sourceContextId = msg.payload[1];
		int sourceTreeIndex = msg.payload[2];

		msg.payload.clear();

		LOG(V3_VERB, "SWEEP MSG [%i] <---?---- [%i](%i) \n", _my_rank, sourceRank, sourceLocalId);

		if (_terminate_all) {
			LOG(V3_VERB, "SWEEP Not answering to post-termination MPI steal request from [%i](%i) \n", sourceRank, sourceLocalId);
			return;
		}

		auto locally_stolen_work = stealWorkFromAnyLocalSolver(sourceRank, sourceLocalId);

		msg.payload = std::move(locally_stolen_work);
		msg.payload.push_back(sourceLocalId);

		//send back to source
		msg.tag = TAG_RETURNING_STEAL_REQUEST;
		int sourceIndex = getJobComm().getInternalRankOrMinusOne(sourceRank);
		msg.treeIndexOfDestination = sourceIndex;
		msg.contextIdOfDestination = getJobComm().getContextIdOrZero(sourceIndex);

		if (msg.contextIdOfDestination != 0) {
			assert(msg.contextIdOfDestination == sourceContextId);
		} else {
			msg.contextIdOfDestination = sourceContextId;
			LOG(V1_WARN,"WARN SWEEP: SEARCHING_WORK receiver [%i] does not know contextIdOfDestination of sender [%i], now using ContextId %i provided by incoming msg itself\n", _my_rank, sourceRank, sourceContextId);
		}

		if (msg.treeIndexOfDestination != -1) {
			assert(msg.treeIndexOfDestination == sourceTreeIndex);
		} else {
			msg.treeIndexOfDestination = sourceTreeIndex;
			LOG(V1_WARN,"WARN SWEEP: SEARCHING_WORK receiver [%i] does not know treeIndexOfDestination of sender [%i], now using treeIndex %i provided by incoming msg itself\n", _my_rank, sourceRank, sourceTreeIndex);
		}

		//todo: solve situation when receiving rank doesnt know yet sending rank
		//probably happens when sweep message slips right into the ongoing ranklist update that is periodically started as aggregating, in job.cpp 233
		assert(msg.contextIdOfDestination != 0 ||
			log_return_false("SWEEP STEAL ERROR: invalid contextIdOfDestination==0. In TAG_RETURNING_STEAL_REQUEST, wanted to return an message"
					"With sourceRank=%i, sourceIndex=%i, payload.size()=%i \n", sourceRank, sourceIndex, msg.payload.size()));

		assert(msg.treeIndexOfDestination != -1 ||
			log_return_false("SWEEP STEAL ERROR: treeIndexOfDestination==-1. In TAG_RETURNING_STEAL_REQUEST, wanted to return an message"
					"With sourceRank=%i, sourceIndex=%i, contextIdOfDestination=%i, payload.size()=%i \n", sourceRank, sourceIndex, msg.contextIdOfDestination, msg.payload.size()));

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
		tryReportUnsat();
	}
	else if (mpiTag == MSG_NOTIFY_JOB_ABORTING)    {LOG(V1_WARN, "SWEEP MSG WARN [%i]: received NOTIFY_JOB_ABORTING \n", _my_rank);}
	else if (mpiTag == MSG_NOTIFY_JOB_TERMINATING) {LOG(V1_WARN, "SWEEP MSG WARN [%i]: received NOTIFY_JOB_TERMINATING \n", _my_rank);}
	else if (mpiTag == MSG_INTERRUPT)			   {LOG(V1_WARN, "SWEEP MSG WARN [%i]: received MSG_INTERRUPT \n", _my_rank);}
	else {LOG(V1_WARN, "SWEEP MSG WARN [%i]: received unexpected mpiTag %i with msg.tag %i \n", _my_rank, mpiTag, msg.tag);}
}

void SweepJob::appl_terminate() {
	LOG(V2_INFO, "SWEEP [%i] (job #%i) got TERMINATE signal (appl_terminate()) \n", _my_rank,getId());
	_terminate_all = true;
	_external_termination = true;
	triggerTerminations();
}


void SweepJob::appl_memoryPanic() {
	LOG(V1_WARN, "[WARN] SWEEP [%i]: Memory panic! \n",_my_rank);
}

bool SweepJob::appl_isDestructible() {

	int _running_sweepers = _started_sweepers_count - _finished_sweepers_count;
	if (_finished_sweepers_count < _nThreads) {
		LOG(V4_VVER, "SWEEP TERM #%i [%i] isDestructible? no. only %i/%i finished, %i running \n",  getId(),_my_rank, _finished_sweepers_count.load(), _nThreads, _running_sweepers);
		return false;
	}

	//all background workers are completely done, so joining them now should happen immediately (even doing it sequentially)
	LOG(V2_INFO, "SWEEP TERM #%i [%i] isDestructible? yes. now joining... \n",  getId(),_my_rank);
	int i=0;
	for (auto &bg_worker : _bg_workers) {
		if (bg_worker->isRunning()) {
			LOG(V4_VVER, "SWEEP TERM #%i [%i] joining bg_worker    (%i) \n",  getId(),_my_rank, i);
			bg_worker->stop();
			LOG(V4_VVER, "SWEEP TERM #%i [%i] joined  bg_worker (%i) \n",  getId(),_my_rank, i);
		}
		i++;
	}
	LOG(V4_VVER, "SWEEP TERM #%i [%i] isDestructible? yes. all joined \n",  getId(),_my_rank);
	return true;
}


void SweepJob::checkForUnsatResults() {
	//If we have an UNSAT result, send it to the root node
	//Makes life easier when only the root node reports to Mallob and to the log files
	//for additional robustness we keep sending this message repeatedly (Can also be a self-message), even though technically only sending it once should be enough
	if (_do_report_UNSAT_to_root) {
		auto msg = getMessageTemplate();
		msg.tag = TAG_FOUND_UNSAT;
		LOG(V1_WARN, "SWEEP [%i] (job #%i) sending UNSAT to root \n", _my_rank,getId());
		getJobTree().sendToRoot(msg);
	}

}

void SweepJob::tryReportUnsat() {
	assert(_is_root);
	bool expected = false;
	//report exactly once to Mallob, ignore all additional internal UNSAT messages
	if (_root_reported_unsat.compare_exchange_strong(expected, true)) {
		reportSolverResult(nullptr, UNSAT);
	}
}

void SweepJob::reportSolverResult(KissatPtr sweeper, int res) {
	LOG(V2_INFO, "SWEEP JOB [%i] reports sweep result %i to Mallob\n", _my_rank, res);
	assert(_is_root || log_return_false("SWEEP ERROR: non-root tries to report result to mallob\n"));
	assert(_solved_status == -1 || log_return_false("SWEEP ERROR: duplicate attempt to report result to mallob")); //something would be off if we called this function more than once
	std::vector<int> formula;
	if (res==UNSAT) {
		//an UNSAT result doesnt come with a proof and can arrive via MPI from another process, so we don't have access to that particular reporting sweeper, nor do we need it
		assert(sweeper == nullptr);
		formula = {};
	} else if (res==IMPROVED){
		assert(sweeper);
		formula = sweeper->extractPreprocessedFormula();
		LOG(V2_INFO, "SWEEP JOB [%i]: Solution Size %i\n", _my_rank, formula.size());
	} else if (res==UNKNOWN) {
		//Design choice: we don't even send a formula back. if we couldn't improve anything in it
		//since, in the SWEEP app, we stop anyways after sweeping ends
		//and in the SATWITHPRE app, we don't use any reported formula in this case of no progress
		assert(sweeper);
		formula = {};
	} else {
		LOG(V1_WARN, "WARN SWEEP [%i]: unexpected result code %i when reporting to mallob \n", _my_rank, res);
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
		while (_started_sweepers_count < _nThreads) {
			LOG(V5_DEBG, "SWEEP [%i](%i) waits for other solvers (started %i/%i)\n", _my_rank, localId, _started_sweepers_count.load(), _nThreads);
			usleep(5000); //5ms
			if (_terminate_all || _external_termination) {
				LOG(V4_VVER, "SWEEP [%i](%i): terminated while waiting in synchronization \n", _my_rank, localId);
				_running_sweepers_count--;
				_finished_sweepers_count++;
				//added this release, maybe we left some memory somewhere when terminating here early
				// kissat_release(sweeper->solver);
				return;
			}
		}
		_sweepers[localId] = sweeper; //only now expose the solver to the rest of the system, now that we know we start solving
		_started_synchronized_solving = true; //multiple threads will write non-thread-safe to this bool, but all only monotonically to "true"

		LOG(V3_VERB, "SWEEP [%i](%i) START solve() \n", _my_rank, localId);
		int res = sweeper->solve(0, nullptr);
		LOG(V3_VERB, "SWEEP [%i](%i) FINISH solve(). Result %i \n", _my_rank, localId, res);

		//transfer some solver-specific statistics
		auto stats = sweeper->fetchSweepStats();
		_worksweeps[localId] = stats.worksweeps;
		_resweeps_in[localId] = stats.resweeps_in;
		_resweeps_out[localId] = stats.resweeps_out;

		if (sweeper->is_congruencer) {
			printCongruenceStats(sweeper);
		}

		if (res==UNSAT) {
			//Found UNSAT
			assert(kissat_is_inconsistent(sweeper->solver) || log_return_false("SWEEP ERROR: Solver returned UNSAT 20 but is not in inconsistent (==UNSAT) state!\n"));
			LOG(V1_WARN, "SWEEP [%i](%i) found UNSAT! \n", _my_rank, localId);
			if (_is_root) {
				//there had been rare cases where sending a self-message on the root-node crashed the program, so in this case we skip the mpi message
				tryReportUnsat();
			} else {
				_do_report_UNSAT_to_root = true;
			}
		} else if (res==UNKNOWN && _is_root && sweeper->getLocalId()==_representative_localId) {
			//Found either IMPROVED or UNKNOWN
			//To reduce concurrency problems, only a single representative solver on the root node is allowed to report this
			assert(_is_root);
			if (sweeper->hasPreprocessedFormula() ) { //by design only the representative sweeper might have a preprocessed (Improved) formula
				if (_root_reported_unsat) {
					//in rare cases, it can happen that an UNSAT result is found in some other solver, but almost at the same time the root solver also ends and doesnt know this information yet, catch this case here
					LOG(V1_WARN, "SWEEP [%i](%i): wanted to report improvement result, but stopped because other solvers already deduced UNSAT\n", _my_rank, localId);
				}  else {
					//we found an Improved formula via Sweeping
					reportSolverResult(sweeper, IMPROVED);
				}
			} else {
				//The whole sweeping didn't yield any improvements
				reportSolverResult(sweeper, UNKNOWN);
			}
		}

		assert(res==UNKNOWN || res==UNSAT || log_return_false("SWEEP ERROR: solver has returned with unexpected signal %i \n", res));

		//A dedicated solver on the root node print his stats as a representative of all other solvers.
		//Their stats differ slightly between solvers, but especially these global stats are very similar between all of them, so we don't bother aggregating/averaging them
		if (_is_root && localId == _representative_localId) {
			printSweepStats(sweeper, true);
		}

		//If no solver sets UNSAT or IMPROVED, the job will be returned by default as UNKNOWN

		if (_running_sweepers_count==1) { //the last solver should report resweeps, as only then they are gathered from all exited solvers
			printResweeps();
		}

		_sweepers[localId]->cleanUp(); //write kissat timing profile
		_sweepers[localId].reset();  //this should delete the only persistent shared pointer on the solver, and thus trigger its destructor soon
		_running_sweepers_count--;
		_finished_sweepers_count++;
		LOG(V3_VERB, "SWEEP [%i](%i) WORKER EXIT, %i left \n", _my_rank, localId, _running_sweepers_count.load());

		// if (_localId==_representative_localId) {
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
	LOG(V3_VERB, "SWEEP [%i](%i) kissat init %.2f ms (%i/%i started)\n", _my_rank, localId, init_dur_ms, _started_sweepers_count.load(), _nThreads);
	if (init_dur_ms > WARN_init_dur) {
		LOG(V1_WARN, "SWEEP WARN STARTUP [%i](%i): kissat init took unusually long, %f ms !\n", _my_rank, localId, init_dur_ms);
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
    sweeper->set_option("profile", max(_params.satProfilingLevel.val, 0)); // do detailed profiling how much time we spent where. kissat allows down to 0, mallob down to -1
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
	sweeper->set_option("mallob_growing_environments", _params.sweepMaxGrowthIteration.val > 1);

	if (_params.sweepCongruence() && _is_root && localId == _congruence_localId) {
		//Do congruence closure instead of sweeping. I.e., syntactical instead of semantical search for equivalences.
		//the congruencer does not participate in workstealing and might be out-of-sync with the sweepers in terms of rounds, so there is no sensible "idle" state
		//we give authority to the sweepers and when they finish the congruencer must finish as well, implemented here by always marking it as idle
		sweeper->set_option("mallob_is_congruencer", 1);
		sweeper->set_option("mallob_is_shweeper",1);
		sweeper->set_option("quiet", 1);
		sweeper->set_option("log", 0);   //0..5
		sweeper->sweeper_is_idle = true;
		sweeper->is_congruencer = true;
	}

	//Own options of Kissat
	//identical to standard options right now
	sweeper->set_option("sweepcomplete", 1);      //deactivates checking for time limits during sweeping, so we dont get kicked out due to some limits

	//this grows exponentially for 6 rounds!
  	sweeper->set_option("sweepclauses", 1024);		//	1024, 0, INT_MAX,	"environment clauses")
  	sweeper->set_option("sweepmaxclauses", 32768);	//	32768,2, INT_MAX,	"maximum environment clauses")

  	sweeper->set_option("sweepdepth", 2);			//, 2,    0, INT_MAX,	"environment depth")
  	sweeper->set_option("sweepmaxdepth", 3);		//	3,    1, INT_MAX,	"maximum environment depth")

	//this grows exponentially for 5 rounds!
  	sweeper->set_option("sweepvars", 256);			//  256,  0, INT_MAX,	"environment variables")
  	sweeper->set_option("sweepmaxvars", 8192);		//	8192, 2, INT_MAX,	"maximum environment variables")

  	sweeper->set_option("sweepfliprounds", 1);		//	1,    0, INT_MAX,	"flipping rounds")
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

	int ranks = getVolume();
	int n_sweepers = ranks * _nThreads;
	int eqs_per_sweeper = stats.sweep_eqs / n_sweepers;
	int units_per_sweeper = stats.sweep_units / n_sweepers;

	double vars_fixed_percent = 100*vars_fixed_end/(double)stats.vars_active_orig;
	double vars_remain_percent = 100*vars_remain_end/(double)stats.vars_active_orig;
	double clauses_removed_percent = 100*clauses_removed/(double)sweeper->_setup.numOriginalClauses;

	LOG(			  V2_INFO, "SWEEP solver [%i](%i) reports statistics in dedicated .sweep file \n", _my_rank, sweeper->getLocalId());
	LOGGER(_reslogger,V2_INFO, "\n");
	LOGGER(_reslogger,V2_INFO, "Reported by [%i](%i) \n", _my_rank, sweeper->getLocalId());
	if (!full) {
		LOGGER(_reslogger,V2_INFO, "SWEEP_ITERATION				%i / %i \n", _root_sweep_iteration, _params.sweepIterations());
		LOGGER(_reslogger,V2_INFO, "SWEEP_TIME					%f seconds \n", Timer::elapsedSeconds() - _start_sweep_timestamp);
		LOGGER(_reslogger,V2_INFO, "SWEEP_UNITS_NEW				%i \n", stats.units_new);
		LOGGER(_reslogger,V2_INFO, "SWEEP_EQUIVALENCES			%i \n", stats.sweep_eqs);
		LOGGER(_reslogger,V2_INFO, "SWEEP_VARS_FIXED_N			%i / %i (%.3f %)\n", vars_fixed_end, stats.vars_active_orig, vars_fixed_percent);
		// LOGGER(_reslogger,V2_INFO, "SWEEP_IMPORTED_UNITS		%i / %i \n", stats.units_useful, stats.units_seen);
		// LOGGER(_reslogger,V2_INFO, "SWEEP_IMPORTED_EQS			%i / %i \n", stats.eqs_useful, stats.eqs_seen); //representative for the reporting solver
		LOGGER(_reslogger,V2_INFO, "SWEEP_TOTAL_SHARED_UNITS %i \n", _root_total_shared_units);
		LOGGER(_reslogger,V2_INFO, "SWEEP_TOTAL_SHARED_EQS   %i \n", _root_total_shared_eqs);
	}

	if (full) {
		LOGGER(_reslogger,V2_INFO, "SWEEP_UNITS_PER_SWEEPER %i \n", units_per_sweeper);
		LOGGER(_reslogger,V2_INFO, "SWEEP_EQS_PER_SWEEPER   %i\n", eqs_per_sweeper);
		LOGGER(_reslogger,V2_INFO, "SWEEP_PRIORITY       %.3f\n", _params.preprocessSweepPriority.val);
		LOGGER(_reslogger,V2_INFO, "SWEEP_PROCESSES      %i\n", getVolume());
		LOGGER(_reslogger,V2_INFO, "SWEEP_THREADS_PER_P  %i\n", _nThreads);
		LOGGER(_reslogger,V2_INFO, "SWEEP_SHARING_PERIOD %.3f sec \n", _params.sweepSharingPeriod.val);
		LOGGER(_reslogger,V2_INFO, "SWEEP_VARS_ORIG		 %i\n", sweeper->_setup.numVars);
		LOGGER(_reslogger,V2_INFO, "SWEEP_VARS_END		 %i\n", stats.vars_end);
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
}

void SweepJob::printCongruenceStats(KissatPtr sweeper) {
	auto stats = sweeper->fetchSweepStats();
	LOGGER(_reslogger, V2_INFO, "CONGRUENCE_EQUIVALENCES   %i \n", stats.congr_eqs);
	LOGGER(_reslogger, V2_INFO, "CONGRUENCE_UNITS          %i \n", stats.congr_units);
	LOGGER(_reslogger, V2_INFO, "CONGRUENCE_EQS_SKIPPED    %i / %i \n", stats.congr_eqs_skipped, stats.eqs_seen);
}


void SweepJob::printIdleFraction() {
	if (_terminate_all) return; //prevent segfault! the sweeper references are being concurrently deleted right now, no touching them

	int idles = 0;
	int active = 0;
	std::ostringstream oss;
	for (auto &sweeper : _sweepers) {
		if (sweeper) {
			active++;
			if (sweeper->sweeper_is_idle) {
				idles++;
				oss << "," << sweeper->getLocalId();
			}
		}
	}
	_lastIdleCount = idles;
	LOG(V3_VERB, "SWEEP  Started %i, Active %i, Running %i, Idle %i: %s \n",  _started_sweepers_count.load(), active, _running_sweepers_count.load(), idles, oss.str().c_str());
	// if (active>0)
		// LOG(V3_VERB, "SWEEP Active %i, Running %i, Idle %i, Started %i. idle nrs: %s \n", active, _running_sweepers_count.load(), idles,  _started_sweepers_count.load(), oss.str().c_str());
}

void SweepJob::checkSharingDelay() {
	if (_terminate_all) return;
	if (!_started_synchronized_solving) return;

	constexpr float MAX_DELAY_FACTOR = 6;
	constexpr float WARNING_PERIOD = 0.5;
	float time = Timer::elapsedSeconds();

	float period = _params.sweepSharingPeriod.val;
	if (!_time_contributed.empty()) {
		float delay = time - _time_contributed.back();
		if (delay > period*MAX_DELAY_FACTOR) {
			//We log two times. Once in the main log file to see the information chronologically correct interleaved with the other logs
			//and onces separately in a .warn file for faster grepping during post-processing, where we would like to avoid to grep through the main logs
			LOG(				V1_WARN, "WARN SWEEP SHARINGDELAY [%i]: %.2f sec since contrib, factor %.1f \n", _my_rank, delay, delay/period);
			LOGGER(_warnlogger, V1_WARN, "WARN SWEEP SHARINGDELAY [%i]: %.2f sec since contrib, factor %.1f \n", _my_rank, delay, delay/period);
		}
	}
	if (!_time_receive_allred.empty()) {
		float delay = time - _time_receive_allred.back();
		if (delay > period*MAX_DELAY_FACTOR) {
			LOG(			    V1_WARN, "WARN SWEEP SHARINGDELAY [%i]: %.2f sec since recv, factor %.1f \n", _my_rank, delay, delay/period);
			LOGGER(_warnlogger, V1_WARN, "WARN SWEEP SHARINGDELAY [%i]: %.2f sec since recv, factor %.1f \n", _my_rank, delay, delay/period);
		}
	}
}

bool SweepJob::okToTrackSharingDelay() {
	//we would get false positives if we already start timing when the solvers haven't even started yet
	if (!_started_synchronized_solving)
		return false;
	//we also would get false positive if the solvers have technically started, but haven't received any initial work yet
	//this second consideration only works at the root node, the other nodes don't have such a live update whether the work for this iteration has been provided yet
	if (_is_root && !_root_provided_initial_work)
		return false;
	return true;
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
	LOGGER(_reslogger, V2_INFO, "[%i] SWEEP_WORKSWEEPS   %i \n", _my_rank, worksweeps);
	LOGGER(_reslogger, V2_INFO, "[%i] SWEEP_RESWEEPS_ALL %i \n", _my_rank, resweeps_in + resweeps_out);
	LOGGER(_reslogger, V3_VERB, "[%i] SWEEP_RESWEEPS_IN  %i \n", _my_rank, resweeps_in);
	LOGGER(_reslogger, V3_VERB, "[%i] SWEEP_RESWEEPS_OUT %i \n", _my_rank, resweeps_out);
}

void SweepJob::TryWorkstealLocal() {
	if (_terminate_all) return;
	//We try to find local work for the requests, if it succeeds it avoids any MPI messaging
	//Can find work if new local work appeared from the time the request was posted to now
	for (auto &request : _worksteal_requests) {
		if (!request.sent) {
			int senderLocalId = request.senderLocalId;
			auto stolen_work = stealWorkFromAnyLocalSolver(_my_rank, senderLocalId);
			if ( ! stolen_work.empty()) {
				request.stolen_work = std::move(stolen_work);
				request.sent = true;
				request.got_steal_response = true;
				request.targetRank = _my_rank; //just for cleaner logging
				request.targetIndex = _my_index;
				//Successful local steal
				LOG(V3_VERB, "SWEEP MSG [%i](%i) <==%i==== localsteal via mainthread  \n", _my_rank, senderLocalId, request.stolen_work.size());
			}
		}
	}
}

void SweepJob::TryWorkstealMPI() {
	if (_terminate_all) return;

	if (getJobComm().size() < getVolume()) {
		LOG(V4_VVER, "SWEEP [%i] Skip MPI workstealing, jobcomm size %i < volume %i\n", _my_rank, getJobComm().size(), getVolume());
		return;
	}

	//Worksteal requests need to be send by the MPI *main* thread, as it can be problematic if the kissat-threads themselves issue MPI messages on their own, since this clashes somehow with the MPI structure/hierarchy
	//Each solver-thread writes a request in the shared memory, and the main MPI thread picks up the request and sends it
	for (auto &request : _worksteal_requests) {
		if (!request.sent) {
			int senderLocalId = request.senderLocalId;

			//There was no local work available, now we prepare to send out an MPI message
			int my_comm_rank = getJobComm().getWorldRankOrMinusOne(_my_index);
			if (my_comm_rank == -1) {
				LOG(V3_VERB, "SWEEP SKIP own rank [%i] (myindex %i) <ctx %i> not yet in JobComm of size %i \n", _my_rank, _my_index, _my_ctx_id, getJobComm().size());
				continue;
			}
			if (_terminate_all || getVolume()==0) {
				request.stolen_work = {};
				request.sent = true;
				request.got_steal_response = true;
				LOG(V3_VERB, "SWEEP [%i](%i) exit steal loop (2nd check, getVolume()==%i)\n", _my_rank, senderLocalId, getVolume());
				break;
			}

			assert(getVolume()>=1 || log_return_false("SWEEP ERROR [%i](%i) in workstealing: getVolume()==%i, i.e. no volume available to steal from\n", _my_rank, senderLocalId, getVolume()));
			if (getVolume()==1) {
				LOG(V4_VVER, "SWEEP SKIP [%i](%i) steal loop --> we are the only MPI rank, no global steal \n", _my_rank, senderLocalId);
				continue;
			}

			assert(request.targetIndex==-1);
			assert(request.targetRank==-1);


			//we want randomized stealing to avoid any local buildup. To be sure that we check all possible other processes, we permute their list
			std::vector<int> tree_indices = std::vector<int>(getVolume());
			for (int i=0; i<getVolume(); i++) {
				tree_indices[i] = i;
			}
			static thread_local std::mt19937 rng(std::random_device{}()); //created/seeded only once per main mallob thread, then just advancing rng calls
			std::shuffle(tree_indices.begin(), tree_indices.end(), rng);

			for (int targetIndex : tree_indices) {
				if (targetIndex==_my_index) {
					// not stealing from ourselves, roll again, and don't count this roll
					continue;
				}
				int targetRank = getJobComm().getWorldRankOrMinusOne(targetIndex);
				if (targetRank == -1) {
					//target rank of this targetIndex is not yet in JobTree, might need some more milliseconds to update, roll again
					LOG(V3_VERB, "SWEEP SKIP target idx %i not in JobComm (size %i) \n", targetIndex, getJobComm().size());
					continue;
				}
				if (getJobComm().getContextIdOrZero(targetIndex)==0) {
					//target is not yet listed in address list. Might happen for a short period just after it is spawned. roll again
					LOG(V3_VERB, "SWEEP SKIP ctx_id of target is missing. getVolume()=%i, rndTargetIndex=%i, rndTargetRank=%i, myIndex=%i, myRank=%i, JobComm size %i \n", getVolume(), targetIndex, targetRank, _my_index, _my_rank, getJobComm().size());
					continue;
				}
				request.targetIndex = targetIndex;
				request.targetRank = targetRank;
				break;
			}

			if (request.targetIndex==-1 || request.targetRank==-1) {
				//couldn't find a target for this request, skip it for now and process the next
				LOG(V4_VVER, "SWEEP MSG [%i](%i) ------> X  (no target possible yet)  \n", _my_rank, request.senderLocalId, request.targetRank);
				continue;
			}

			request.sent = true;
			JobMessage msg = getMessageTemplate();
			msg.tag = TAG_SEARCHING_WORK;
			//Need to add these two fields because we are doing arbitrary point-to-point communication
			msg.treeIndexOfDestination = request.targetIndex;
			msg.contextIdOfDestination = getJobComm().getContextIdOrZero(request.targetIndex);
			assert(msg.contextIdOfDestination != 0 || log_return_false("SWEEP ERROR: contextIdOfDestination==0 in workstealing request! Source rank=%i, targetRank %i \n", _my_rank, request.targetRank));
			//We also send our contextId. Because it can happen that the receiving rank does not yet know our contextId,
			//which (probably) happens when a worksteal request is sent right when also the ranklist aggregation update is propagating through all ranks, where some (like this rank here) are already updated, while others (like the receiving rank) aren't.
			//So as a backup for the receiving rank, we also provide our contextId
			int myContexId = getJobComm().getContextIdOrZero(_my_index);
			//Update: and we also send our treeIndex for the same reason, the receiver might not yet have our address data
			msg.payload = {request.senderLocalId, myContexId, _my_index};
			assert(msg.payload.size() == NUM_SEARCHING_WORK_FIELDS);
			LOG(V3_VERB, "SWEEP MSG [%i](%i) ---?---> [%i] \n", _my_rank, request.senderLocalId, request.targetRank);
			getJobTree().send(request.targetRank, MSG_SEND_APPLICATION_MESSAGE, msg);
		}
	}
}


void SweepJob::checkForNewImportRound(KissatPtr sweeper) {
	int available_import_round = _available_import_round.load(std::memory_order_acquire);
	int my_last_import_round = sweeper->sweep_import_round;
	if (available_import_round != my_last_import_round) [[unlikely]] {
		//there is new data from a new sharing round
		// publish_round = _sharing_import_round.load(std::memory_order_acquire);
		LOG(V4_VVER, "SWEEP checked %i --> %i \n", my_last_import_round, available_import_round);

		assert(my_last_import_round <= available_import_round);
		if (my_last_import_round!=0 && my_last_import_round != available_import_round - 1) {
			LOG(V1_WARN, "WARN SWEEP: Solver [%i](%i) skipped import rounds, went %i -> %i \n", _my_rank, sweeper->getLocalId(), my_last_import_round, available_import_round);
		}
		sweeper->sweep_import_round  = available_import_round;
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
	if (_terminate_all) { //can happen that we arrive here before the solver learned that we already terminated
		//leave *ilit's untouched
		return;
	}


	//todo: this localId can also just be a congruencer! lives in the same array, interfaces the same way with eq/units
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
	assert(*ilit1 < *ilit2										|| log_return_false("SWEEP ERROR: in cbImportEq: *ilit1 %i is larger than %i *ilit2, but they should be sorted (index %i, %i,)\n", *ilit1, *ilit2, idx, idx+1));
	assert(sweeper->sweep_EQS_index <= sweeper->sweep_EQS_size	|| log_return_false("SWEEP ERROR: in Equivalence Import: index %i is now beyond size %i \n", sweeper->sweep_EQS_index.load(), sweeper->sweep_EQS_size.load()));
	//now returning to kissat solver
}

int SweepJob::cbCustomQuery(int query) { //general interface to communicate some simple integers between solver and Mallob, without the need to declare yet another function each time
	if (query==QUERY_SWEEP_ITERATION) {
		return _root_sweep_iteration;
	}

	return 0;
}

void SweepJob::cbImportUnit(int *ilit, int localId) {
	if (_terminate_all) {
		return;
	}
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
	_root_provided_initial_work = true;
}




void SweepJob::cbSearchWorkInTree(unsigned **work, int *work_size, int localId) {
	KissatPtr sweeper = _sweepers[localId]; //this array access is safe because the callback is called by this sweeper itself
	sweeper->work_received_from_steal = {};

	//setting this sweeper to idle is no longer done directly here, because it caused a race condition for the very first solver that was marked idle while it was receiving the initial work

	//This is a fake loop, we only traverse it once. we use the loop syntax to easily allow us to break out at several points and jump to the bottom, i.e. makeshift gotos
	while (true) {
		if (_terminate_all) {
			sweeper->work_received_from_steal = {};
			sweeper->sweeper_is_idle = true;
			LOG(V3_VERB, "Sweeper [%i](%i) exit steal loop\n", _my_rank, localId);
			//make extra sure that this sweeper receives the termination signal (yet again)
			sweeper->triggerSweepTerminate();
			sweeper->count_repeated_missed_termination++;
			if (sweeper->count_repeated_missed_termination % sweeper->WARN_ON_REPEATED_MISSED_TERMINATION==0) {
				LOG(V3_VERB, "WARN Sweeper [%i](%i) in %i-th worksteal loop after termination\n", _my_rank, localId, sweeper->count_repeated_missed_termination);
			}

			break;
		}

		//Skip stealing if there hasn't been work provided, would be pointless to search for it
		//Furthermore, serve initial work to the representative solver (typically localId 0) at the root node.
		//This is hardcoded to prevent any concurrency problems that would occur if the representative solver would be deduced during runtime. the [root](0) solver is always assumed to exist
		if (_is_root && ! _root_provided_initial_work) {
			if (localId==_representative_localId) {
				provideInitialWork(sweeper);
			}
			break;
		}

		//First strategy: steal locally from shared memory
		LOG(V5_DEBG, "SWEEP WORK [%i](%i) steal loop --> local steal \n", _my_rank, localId);
		sweeper->sweeper_is_idle = true;
		auto stolen_work = stealWorkFromAnyLocalSolver(_my_rank, localId);

		if ( ! stolen_work.empty()) {
			//Successful local steal
			//store the steal data persistently in C++, such that C can keep operating on that memory segment
			sweeper->work_received_from_steal = std::move(stolen_work);
			LOG(V3_VERB, "SWEEP MSG [%i](%i) <==%i==== localsteal direct  \n", _my_rank, localId, sweeper->work_received_from_steal.size(), _my_rank);
			break;
		}


		if ( ! _started_communication) {
			LOG(V3_VERB, "SWEEP [%i](%i) Skip MPI request, not communicating yet\n", _my_rank, localId);
			break;
		}
		//Second strategy: steal globally via MPI
		//We deposit a request for an MPI message via shared memory, and the main MPI thread of this process will pick up the request and actually send it via MPI
		//This extra step is necessary, because the thread is just "some" solver-thread and it can cause problems it it start sending MPI messages on it's own

		// LOG(V3_VERB, "SWEEP MSG [%i](%i) ----?---> req  \n", _my_rank, localId);
		_worksteal_requests[localId].newBlankRequest(localId);

		//Wait here until we hear back
		//To receive a worksteal answer, main MPI thread scans for incoming messages and passes it to us via shared memory
		constexpr unsigned WAIT_MICROSEC=100;
		unsigned reps=0;
		while(_worksteal_requests[localId].got_steal_response == false) {
			usleep(WAIT_MICROSEC);
			reps++;
			if (reps%512==0) {
				LOG(V1_WARN, "SWEEP WARN [%i](%i) waiting for MPI steal response since %i ms \n", _my_rank, localId, (reps*WAIT_MICROSEC)/1000);
			}
			if (_terminate_all) {
				LOG(V3_VERB, "SWEEP [%i](%i) exit wait loop\n", _my_rank, localId);
				break;
			}
		}

		if (_terminate_all) {
			sweeper->sweeper_is_idle = true;
			break;	 //skip touching the work array at all, weird stuff can happen when we are already in the termination stage
		}

		//Successful steal if size > 0
		if (_worksteal_requests[localId].stolen_work.size()>0) {
			sweeper->work_received_from_steal = std::move(_worksteal_requests[localId].stolen_work);
			LOG(V4_VVER, "SWEEP recv [%i](%i) <==%i==== [%i] \n",  _my_rank, localId, sweeper->work_received_from_steal.size(), _worksteal_requests[localId].targetRank);
			break;
		}
		//always exit this fake loop. the real loop is in the kissat solver, which continuously calls this function
		break;
	}

	//we control the memory on C++/Mallob Level, and only tell the kissat solver where it can find the work (or where it can read the zero)
	//the solver will also write on this array, but only within the allocated bounds provided by C++/Mallob
	*work = reinterpret_cast<unsigned int*>(sweeper->work_received_from_steal.data());
	*work_size = sweeper->work_received_from_steal.size();
	assert(*work_size>=0);
	if (*work_size>0) {
		// LOG(V3_VERB, "SWEEP [%i](%i) received %i work \n", _my_rank, localId, *work_size);
		sweeper->sweeper_is_idle = false; //we keep solver marked as "idle" once the whole solving is terminated, otherwise race-conditions can occur where suddenly the solver is treated as active again
	}
	//update: we no longer send the termination information via this worksteal callback, but separately
	// if (_terminate_all) {
		// LOG(V3_VERB, "SWEEP [%i](%i) returning to solver with termination info\n", _my_rank, localId);
	// }
	//The thread now returns to the kissat solver
}

void SweepJob::rootInitiateNewSharingRound() {
	if (!_is_root)
		return;
	if (_terminate_all)
		return;

	assert(_is_root); //only the root node initiates sharing rounds
	if (!_bcast) {
		LOG(V1_WARN, "SWEEP WARN : SHARE BCAST root couldn't initiate sharing round, _bcast is Null\n");
		return;
	}

	if (Timer::elapsedSeconds() < _root_last_sharing_start_timestamp + _params.sweepSharingPeriod.val) {
		//not yet time for next sharing round
		return;
	}

	if (!_started_synchronized_solving) {
		LOG(V3_VERB, "SWEEP root: Delaying first sharing round, not all solvers online yet (%i/%i) \n", _started_sweepers_count.load(), _nThreads);
		return;
	}

	//make sure that only one sharing operation is going on at a time
	//on this root node, hasReceivedBroadcast is equivalent to asking whether this _bcast object has already started a broadcast
	if (_bcast->hasReceivedBroadcast()) {
		LOG(V3_VERB, "SWEEP root: Delaying new sharing round, old round is still ongoing\n");
		return;
	}
	//Broadcast a ping to all workers to initiate an AllReduce
	//The broadcast includes all workers currently reachable by the root-node and informs them about their parent and potential children
	//It then causes the leaf nodes to call the callback, initiating the AllReduce
	_root_last_sharing_start_timestamp = Timer::elapsedSeconds();
	_time_start_bcast.push_back(_root_last_sharing_start_timestamp);
	LOG(V3_VERB, "SWEEP root: Initiating new sharing round via modular broadcast\n");
	JobMessage msg = getMessageTemplate();
	msg.tag = _bcast->getMessageTag();
	msg.payload = {};
	_bcast->broadcast(std::move(msg));
}

void SweepJob::appendMetadataToReductionElement(std::vector<int> &contrib, int is_idle, int unit_size, int eq_size) {
	contrib.insert(contrib.end(), NUM_METADATA_FIELDS, 0); //Make space for the upcoming metadata, initialized with zero
	int size = contrib.size();
	int n=0;
	n++; contrib[size - METADATA_TERMINATE]      = 0;  //dummy, will be set by root transformation, here just for completeness
	n++; contrib[size - METADATA_SWEEP_ITERATION]= 0;  //dummy, ""
	n++; contrib[size - METADATA_SHARING_ROUND]  = 0;  //dummy, ""
	n++; contrib[size - METADATA_IDLE]       = is_idle;
	n++; contrib[size - METADATA_UNIT_SIZE]  = unit_size;
	n++; contrib[size - METADATA_EQ_SIZE]    = eq_size;
	assert(n==NUM_METADATA_FIELDS || log_return_false("SWEEP ERROR: Added metadata count (%i) doesnt match expected number (%i) \n", n, NUM_METADATA_FIELDS));
}

void SweepJob::cbContributeToAllReduce() {
	assert(_bcast);
	assert(_bcast->hasResult());
	//bcast hasResult present means that this Process got responses from all its children, so the tree structure is correctly known, and we can continue with a contribution and reduction

	LOG(V4_VVER, "SWEEP BCAST Callback to AllReduce\n");
	auto snapshot = _bcast->getJobTreeSnapshot();

	LOG(V4_VVER, "SWEEP BCAST Callback, snapshot: nbChildren %i. leftChild (%i)[%i], rightChild (%i)[%i]  \n",
		snapshot.nbChildren, snapshot.leftChildIndex, snapshot.leftChildNodeRank, snapshot.rightChildIndex, snapshot.rightChildNodeRank);

	if (! _is_root) {
		LOG(V4_VVER, "SWEEP [%i] RESET non-root BCAST, prepares for next sharing bcast \n", _my_rank);
		//Prepare all non-root processes to be ready to receive the next broadcast
		_bcast.reset(new JobTreeBroadcast(getId(), snapshot, [this]() {cbContributeToAllReduce();}, TAG_BCAST_INIT));
	}

	if (_terminate_all) {
		LOG(V4_VVER, "SWEEP BCAST skip reduction, status is already _terminate_all\n");
		return;
	}

	JobMessage baseMsg = getMessageTemplate();
	baseMsg.tag = TAG_ALLRED;
	LOG(V4_VVER, "SWEEP [%i] RESET AllReduction, using bcast snapshot, to prepare contributing local data. \n", _my_rank);
	_red.reset(new JobTreeAllReduction(snapshot, baseMsg, std::vector<int>(), aggregateEqUnitContributions));
	if (_is_root)
		_red->setInplaceTransformationOfElementAtRoot(_inplace_rootTransform);


	//Bring individual data per thread in the sharing element format: [Equivalences, Units, eq_size, unit_size, all_idle]
	std::list<std::vector<int>> contribs;
	int id=-1; //for debugging
	for (auto &sweeper : _sweepers) {
		id++;
		if (!sweeper) {
			LOG(V5_DEBG, "SWEEP [%i](%i) not yet initialized, skipped in contribution aggregation \n", _my_rank, id);
			continue;
		}

		//Mutex, because the solvers are asynchronously pushing all the time new eqs&units into these vectors (including reallocations after push_back), make sure that doesn't happen while we copy/move
		std::vector<int> eqs, units;
		{
			std::lock_guard<std::mutex> lock(sweeper->sweep_export_mutex);
			eqs = std::move(sweeper->eqs_to_share);
			units = std::move(sweeper->units_to_share);
			if (!eqs.empty() || !units.empty()) {
				LOG(V5_DEBG, "        [%i](%i) Export:  %i E  %i U\n", _my_rank, sweeper->getLocalId(), eqs.size()/2, units.size());
			}

			//by moving we also clear their current position, i.e. prevents from sharing the data twice
		}

		int eq_size = eqs.size();
		int unit_size = units.size();
		assert(eq_size%2==0 || log_return_false("ERROR in AGGR: Non-even number %i of equivalence literals, should always come in pairs", eq_size)); //equivalences come always in pairs
		//we need to glue together equivalences and units. can use move on the equivalences to save a copying of them, and only need to copy the units
		//moved logging before the actions, because this code triggered std::bad_alloc once, might give some more info next time
		LOG(V5_DEBG, "SWEEP SHARE REDUCE (%i): %i eq_size, %i units, %i idle \n", sweeper->getLocalId(), eq_size, unit_size, sweeper->sweeper_is_idle);
		std::vector<int> contrib = std::move(eqs);
		contrib.insert(contrib.end(), units.begin(), units.end());

		appendMetadataToReductionElement(contrib, sweeper->sweeper_is_idle, unit_size, eq_size);

		contribs.push_back(contrib);
	}

	auto aggregation_element = aggregateEqUnitContributions(contribs);

	LOG(V4_VVER, "SWEEP [%i] contributing ~~~%i~~~(+%i)~~> to _red \n", _my_rank, aggregation_element.size()-NUM_METADATA_FIELDS, NUM_METADATA_FIELDS);

	if (_terminate_all) {
		LOG(V4_VVER, "SWEEP SHARE BCAST skip contribution, seen already _terminate_all\n");
		return;
	}

	if (okToTrackSharingDelay()) {
		_time_contributed.push_back(Timer::elapsedSeconds());
	}

	_red->contribute(std::move(aggregation_element));

}

void SweepJob::advanceAllReduction() {
	if (!_red)
		return;
	// LOG(V4_VVER, "SWEEP [%i] advance() \n", _my_rank);
	//we always keep the global reduction advancing, independently of the state of the local solvers
	_red->advance();
	if (!_red->hasResult())
		return;

	//There is data from global aggregation, extract it
	auto data = _red->extractResult();
	const int terminate   = data[data.size()-METADATA_TERMINATE];
	const int sweep_iteration = data[data.size()-METADATA_SWEEP_ITERATION];
	const int sharing_round= data[data.size()-METADATA_SHARING_ROUND];
	const int all_idle    = data[data.size()-METADATA_IDLE];
	const int unit_size   = data[data.size()-METADATA_UNIT_SIZE];
	const int eq_size     = data[data.size()-METADATA_EQ_SIZE];
	assert(eq_size%2==0 || log_return_false("SWEEP ERROR: Import Equality size not even, but %i\n", eq_size));


	if (!_started_synchronized_solving && (eq_size>0 || unit_size>0))  {
		LOG(V3_VERB, "SWEEP WARN [%i] (iter %i round %i): Skipping %i eqs, %i units bc local solvers are not all init'd yet \n", _my_rank, sweep_iteration, sharing_round, eq_size/2, unit_size);
	}

	//if our local solvers are not fully initialised yet we ignore the global sharing data
	//but we still read the metadata before to log it
	if (!_started_synchronized_solving)
		return;

	if (okToTrackSharingDelay())
		_time_receive_allred.push_back(Timer::elapsedSeconds());


	if (eq_size > MAX_IMPORT_SIZE) {
		LOG(V1_WARN, "WARN -SWEEP too many equalities to import! %i, max %i\n", eq_size, MAX_IMPORT_SIZE);
	}
	if (unit_size > MAX_IMPORT_SIZE) {
		LOG(V1_WARN, "WARN -SWEEP too many units to import! %i, max %i\n", unit_size, MAX_IMPORT_SIZE);
	}

	for (int i=0; i<eq_size && i < MAX_IMPORT_SIZE; i++) {
		_EQS_to_import[i] = data[i];
	}

	// for (int i=eq_size; i<data.size()-NUM_METADATA_FIELDS; i++) {
	for (int i=0; i<unit_size && i < MAX_IMPORT_SIZE; i++) {
		_UNITS_to_import[i] = data[eq_size + i];
	}

	_EQS_import_size.store(eq_size, std::memory_order_relaxed);
	_UNITS_import_size.store(unit_size, std::memory_order_relaxed);
	_available_import_round.store(sharing_round, std::memory_order_release);

	//the sweepers need to know the current sweep iteration to set the size of their sweep environments accordingly
	//which grow with each round (if activated)
	if (!_terminate_all && sweep_iteration <= _params.sweepMaxGrowthIteration.val) { //when sweepers are already being deleted we don't want to risk a segfault looping over them...
		for (auto &sweeper : _sweepers) {
			if (sweeper) {
				shweep_set_sweep_round(sweeper->solver, sweep_iteration);
			}
		}
	}

	LOG(V2_INFO, "SWEEP RED iter(%i) round(%i) got: %i EQS, %i UNITS, (%i)all_idle, (%i)term. #locally idle: %i/%i \n", sweep_iteration, sharing_round, eq_size/2, unit_size, all_idle, terminate, _lastIdleCount, _nThreads);

	//prepare the next sharing round, which gets started from the root node
	if (_is_root) {
		LOG(V4_VVER, "SWEEP root: rest bcast listen for next sharing round\n", _my_rank);
		_bcast.reset(new JobTreeBroadcast(getId(), getJobTree().getSnapshot(),
			[this]() {cbContributeToAllReduce();}, TAG_BCAST_INIT));
	}

	//Reduction is finished. Contrary to the bcast, we dont need to directly re-create a new reduction object, but can leave it at null.
	//The new reduction object will be only created when needed when a new broadcast is present (and completed)
	_red.reset();


	//We received the termination signal via the app-internal data sharing
	if (terminate) {
		_terminate_all = true;
		//update: we now trigger terminations here directly, no longer indirectly via the worksteal callback
		triggerTerminations();
		LOG(V1_WARN, "# \n # \n # --- [%i] got terminate flag, TERMINATING SWEEP JOB ---\n # \n", _my_rank);
	}

}



std::vector<int> SweepJob::aggregateEqUnitContributions(std::list<std::vector<int>> &contribs) {
	//Each contribution has the format [Equivalences,Units, eq_size,unit_size,all_idle].
	//									-----data---------  ------metadata------------

	//sanity check whether each contribution contains coherent size information about itself
	for (const auto& contrib : contribs) {
		int claimed_eq_size = contrib[contrib.size()- METADATA_EQ_SIZE];
		int claimed_unit_size = contrib[contrib.size()- METADATA_UNIT_SIZE];
		int claimed_total_size = claimed_eq_size + claimed_unit_size + NUM_METADATA_FIELDS;
		assert(contrib.size() == claimed_total_size ||
			log_return_false("ERROR in AllReduce, Bad Element Format: Claims total size %i != %i actual contrib.size() (claims: eq_size %i,  units size %i, metadata %i)", claimed_total_size, contrib.size(), claimed_eq_size, claimed_unit_size, NUM_METADATA_FIELDS)
			);
	}

#define DEDUPLICATE_CONTRIBS 1

#if ! DEDUPLICATE_CONTRIBS

	LOG(V4_VVER, "SWEEP AGGR %i contributions \n", contribs.size());
	size_t total_aggregated_size = NUM_METADATA_FIELDS; //the new element will contain the metadata once at the end
	int i=0;
	for (const auto& contrib : contribs) {
		total_aggregated_size += contrib.size()-NUM_METADATA_FIELDS; //we will copy everything but the metadata from each contribution
		LOG(V4_VVER, "SWEEP AGGR Element %i: contrib.size() %i, w/o metadata %i, curr summed size %i \n", i, contrib.size(), contrib.size()-NUM_METADATA_FIELDS, total_aggregated_size);
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
		int eq_size = contrib[contrib.size()-METADATA_EQ_SIZE];
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
		int eq_size = contrib[contrib.size()-METADATA_EQ_SIZE];  //need to know where the eq ends, i.e. where the units start
		int unit_size = contrib[contrib.size()-METADATA_UNIT_SIZE];
		aggr_unit_size += unit_size;
		aggregated.insert(aggregated.end(), contrib.begin()+eq_size, contrib.end()-NUM_METADATA_FIELDS); //not copying the metadata at the end
		// oss_u << "| " << i << ":" << unit_size << " "	;
		i++;
	}
#else

	//Layout: contrib = [equivalences, units, metadata]

	robin_hood::unordered_flat_set<int> dedup_units;
	robin_hood::unordered_flat_set<uint64_t> dedup_eqs;

	int orig_unit_size = 0;
	int orig_eqs_size = 0;

	//Deduplicate eq & units via hashing
	//each eq-pair represented as one 64-bit key to have an easy datatype
	for (const auto &contrib : contribs) {
		const int eq_size = contrib[contrib.size()-METADATA_EQ_SIZE];
		const int unit_size = contrib[contrib.size()-METADATA_UNIT_SIZE];
		orig_unit_size += unit_size;
		orig_eqs_size  +=eq_size;
		for (int j=0; j < eq_size; j+=2) {
			int lit1 = contrib[j];
			int lit2 = contrib[j+1];
			assert(lit1<lit2);
			uint64_t litpair = (((uint64_t)lit1) << 32) | (uint64_t) lit2;
			dedup_eqs.insert(litpair);
		}
		const int units_start = eq_size;
		const int units_end   = units_start + unit_size;
		for (int j=units_start; j< units_end; j++) {
			dedup_units.insert(contrib[j]);
		}
	}

	//Extract deduplicated units and equivalences
	std::vector<int> aggregated;
	int total_aggregated_size = 0;
	int aggr_eq_size   = 2*dedup_eqs.size(); //each key is 64 bit, i.e. 2 literals
	int aggr_unit_size = dedup_units.size();
	int aggr_data_size = aggr_eq_size + aggr_unit_size;
	aggregated.resize(aggr_data_size);
	int j=0;
	for (uint64_t litpair : dedup_eqs) {
		uint32_t lit1 = litpair >> 32;
		uint32_t lit2 = litpair & 0xffffffff;
		assert(lit1 < lit2);
		aggregated[j++] = lit1;
		aggregated[j++] = lit2;
	}
	for (int unit : dedup_units) {
		aggregated[j++] = unit;
	}
	assert(j==aggr_data_size);
	total_aggregated_size = aggr_data_size + NUM_METADATA_FIELDS;

	// cout << orig_unit_size - aggr_unit_size << " " << orig_eqs_size - aggr_eq_size << endl;

#endif

	//See whether all solvers are idle
	bool all_idle = true;
    for (const auto &contrib : contribs) {
		bool idle = contrib[contrib.size()-METADATA_IDLE];
    	all_idle &= idle;
    }

	if (contribs.empty()) {
		all_idle = false; //edge-case: if not a single solver is initialized yet, we are waiting for them to come online, so they are not really idle
	}


	appendMetadataToReductionElement(aggregated, all_idle, aggr_unit_size, aggr_eq_size);

	if (contribs.size()>1)
		LOG(V3_VERB, "SWEEP RED aggr %i contribs: %i EQ, %i UNITS, %i ALL_IDLE\n", contribs.size(), aggr_eq_size/2, aggr_unit_size, all_idle);
	int individual_sum =  aggr_eq_size + aggr_unit_size + NUM_METADATA_FIELDS;
	assert(total_aggregated_size == individual_sum ||
		log_return_false("SWEEP ERROR: aggregated element assert failed: total_size %i != %i individual_sum (total_eq_size %i + total_unit_size %i + metadata %i) ",
			total_aggregated_size, individual_sum, aggr_eq_size, aggr_unit_size, NUM_METADATA_FIELDS));
    return aggregated;
}



std::vector<int> SweepJob::stealWorkFromAnyLocalSolver(int asking_rank, int asking_sourceLocalId) { //Parameters are only used for verbose logging, don't influence function behaviour
	auto rand_permutation = getRandomIdPermutation();

	for (int localId : rand_permutation) {
		auto stolen_work = stealWorkFromSpecificLocalSolver(localId);
		if ( ! stolen_work.empty()) {
			LOG(V3_VERB, "SWEEP MSG [%i](%i) ====%i==> [%i](%i) \n",_my_rank, localId, stolen_work.size(), asking_rank, asking_sourceLocalId);
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

	// LOG(V3_VERB, "SWEEP stealattempt on [%i](%i)\n", _my_rank, localId);
	//We dont know yet how much there is to steal, so we ask for an upper bound
	//It can also be that the solver we want to steal from is not fully initialized yet
	//For that in the C code there are further guards against unfinished initialization, all returning 0 in that case
	//The congruence closure solver will always return 0, as it doesnt operate on work and doesnt have any
	int max_steal_amount = shweep_get_max_steal_amount(sweeper->solver);
	if (max_steal_amount < MIN_STEAL_AMOUNT)
		return {};

	// LOG(V2_INFO, "ß %i max_steal_amount\n", max_steal_amount);
	assert(max_steal_amount > 0 || log_return_false("SWEEP STEAL ERROR [%i](%i): negative max steal amount %i, maybe segfault into non-initialized kissat solver \n", _my_rank, localId, max_steal_amount));
	assert(max_steal_amount < 2*_numVars || log_return_false("SWEEP STEAL ERROR [%i](%i): too large max steal amount %i >= 2*NUM_VARS, maybe segfault into non-initialized kissat solver \n", _my_rank, localId, max_steal_amount));

	//There is something to steal
	//Allocate memory for the steal here in C++, and pass the allocation to kissat such that it can fill it with the stolen work
	std::vector<int> stolen_work = std::vector<int>(max_steal_amount);

	// LOG(V3_VERB, "[%i] stealing from (%i), expecting max %i  \n", _my_rank, localId, max_steal_amount);
	int actually_stolen = shweep_steal_from_this_solver(sweeper->solver, reinterpret_cast<unsigned int*>(stolen_work.data()), max_steal_amount);
	// LOG(V3_VERB, "ß Steal request got %i actually stolen\n", actually_stolen);
	// LOG(V3_VERB, "SWEEP WORK actually stolen ---%i---> from [%i](%i)\n", localId, stolen_work.size());
	if (actually_stolen == 0)
		return {};
	//We allocated the steal array as large as maximally needed, but that was only an upper limit estimate, often during stealing it turns out that we receive a bit less work than estimated
	//So now we know the exact amount of work we stole, and shrink the array to have its .size() match with this stolen amount
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
	constexpr int BITS_PER_MB = 8000000;
	float formula_in_MB = ((float)payload_size*32)/BITS_PER_MB;
	// LOG(V2_INFO, "SWEEP Loading Formula, size %i \n", payload_size);

	LOG(V4_VVER, "SWEEP [%i](%i) loading formula (%.3f MB) \n", _my_rank, sweeper->getLocalId(), formula_in_MB);
	float t0 = Timer::elapsedSeconds();

	for (int i = 0; i < payload_size ; i++) {
		sweeper->addLiteral(lits[i]);
	}

	float t1 = Timer::elapsedSeconds();
	LOG(V4_VVER, "SWEEP [%i](%i) loaded  formula (%.3f MB) in %.3f ms \n", _my_rank, sweeper->getLocalId(), formula_in_MB , (t1-t0)*1000);
}

void SweepJob::triggerTerminations() {
	LOG(V2_INFO, "SWEEP TERM #%i [%i] trigger solver terminations (ctx %i). State before: Running %i, Finished %i \n", getId(), _my_rank, _my_ctx_id, _running_sweepers_count.load(), _finished_sweepers_count.load());
	int i=0;
	for (auto &sweeper : _sweepers) {
		if (sweeper) {
			sweeper->triggerSweepTerminate();
			LOG(V4_VVER, "SWEEP TERM #%i [%i] trigger termination of solver (%i) \n", getId(), _my_rank, i);
		} else {
			LOG(V4_VVER, "SWEEP TERM #%i [%i] skip    termination of solver (%i), already null \n", getId(), _my_rank, i);
		}
		i++;
	}

	//each sweeper checks constantly for the interruption signal (on the ms scale or faster), allow for gentle own exit
	// while (_started_sweepers_count < _nThreads || _running_sweepers_count>0) {
		// LOG(V4_VVER, "SWEEP TERM #%i [%i] still %i solvers running\n", getId(), _my_rank, _running_sweepers_count.load());
		// int i=0;
		// for (auto &sweeper : _sweepers) {
			// if (sweeper) {
				// sweeper->triggerSweepTerminate();
				// LOG(V4_VVER, "SWEEP TERM #%i [%i] terminating solver (%i)\n", getId(), _my_rank, i);
			// }
			// i++;
		// }
		// usleep(2000);
	// }
	// LOG(V4_VVER, "SWEEP TERM #%i [%i] no more solvers running\n", getId(), _my_rank);

	// usleep(500);

}

SweepJob::~SweepJob() {
	LOG(V4_VVER, "SWEEP JOB DESTRUCTOR ENTERED (ctx %i) \n", _my_ctx_id);
	// triggerTerminations();
	LOG(V4_VVER, "SWEEP JOB DESTRUCTOR DONE\n");
}














