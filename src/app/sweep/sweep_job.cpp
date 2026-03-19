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
	// _rootlogger(Logger::getMainInstance().copy("<ROOT>", ".sweeproot"))
{
	assert(_params.jobCommUpdatePeriod() > 0 || log_return_false("[ERROR] For this application to work,"
            " you must explicitly enable job communicators with the -jcup option, e.g., -jcup=0.1\n"));

	LOG(V2_INFO, "## \n");
	LOG(V2_INFO, "New SweepJob MPI Process on rank [%i] with %i threads, ctx %i \n", getJobTree().getRank(), params.numThreadsPerProcess.val, getJobTree().getContextId());
	LOG(V2_INFO, "## \n");
}


//callback from kissat
void cb_search_work_in_tree(void *SweepJob_state, unsigned **work, int *work_size, int local_id) {
    ((SweepJob*) SweepJob_state)->cbStealWorkNew(work, work_size, local_id);
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

void cb_report_iteration(void *SweepJobState, int localId) {
	return ((SweepJob*)SweepJobState)->cbReportIteration(localId);
}

void SweepJob::appl_start() {
	if (_params.sweepMaxIterations.val==0) {
		LOG(V2_INFO,"Skip SWEEP JOB, as sweepMaxIterations==0");
		return;
	}
	_started_appl_start = true;
	_my_rank = getJobTree().getRank();
	_my_index = getJobTree().getIndex();
	_my_ctx_id = getJobTree().getContextId();
	_is_root = getJobTree().isRoot();
	_nThreads = min( getNumThreads(), _params.numThreadsPerProcess.val);
	if (_nThreads < _params.numThreadsPerProcess.val) {
		LOG(V1_WARN,"SWEEP WARN : cut down threads to %i \n", _nThreads);
	}
	const JobDescription& desc = getDescription();
	int numVars = desc.getAppConfiguration().fixedSizeEntryToInt("__NV");
	int numClauses = desc.getAppConfiguration().fixedSizeEntryToInt("__NC");

	LOG(V2_INFO,"SWEEP JOB SweepJob appl_start() STARTED: Rank %i, Index %i, ContextId %i, is root? %i, Parent-Rank %i, Parent-Index %i, threads=%d, NumVars %i, NumClauses %i\n",
		_my_rank, _my_index, getJobTree().getContextId(), _is_root, getJobTree().getParentNodeRank(), getJobTree().getParentIndex(), _nThreads, numVars, numClauses);
    _metadata = getSerializedDescription(0)->data();

	_start_sweep_timestamp = Timer::elapsedSeconds();

    // _reslogger = Logger::getMainInstance().copy("<RESULT>", ".sweep");
    // _warnlogger = Logger::getMainInstance().copy("<WARN>", ".warn");
    // _rootlogger = Logger::getMainInstance().copy("<ROOT>", ".root");

	_worksteal_requests.resize(_nThreads);

	//Need to mark them as empty initially, such that the solvers can turn them into actual requests when required.
	//Once the chain started, the solvers will mark their processed request as empty again on their own
	for (auto &request : _worksteal_requests) {
		request.is_inactive = true;
	}
	//pre-allocate a fixed array from where solver can concurrently import the received equalities and units
	_EQS_to_import.resize(MAX_IMPORT_SIZE);
	_UNITS_to_import.resize(MAX_IMPORT_SIZE);

	// _worksweeps = std::vector<int>(_nThreads, -1);
	// _resweeps_in = std::vector<int>(_nThreads, -1);
	// _resweeps_out = std::vector<int>(_nThreads, -1);

	//To randomize workstealing on a given rank, we create a list of all ids that will be then shuffled each time
	std::ostringstream oss;
	for (int localId=0; localId < _nThreads; localId++) {
		_list_of_ids.push_back(localId);
		oss << localId << ",";
	}
	LOG(V3_VERB,"SWEEP LIST_OF_LOCAL_IDS: %s \n", oss.str().c_str());

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
	LOG(V3_VERB,"SWEEP Create solvers\n");
	for (int localId=0; localId < _nThreads; localId++) {
		createAndStartNewSweeper(localId);
	}

	//set some general metadata information
	_internal_result.id = getId();
	_internal_result.revision = getRevision();



	LOG(V3_VERB, "SWEEP appl_start() FINISHED\n");
}



// Called periodically by the main thread to allow the worker to emit messages. Must exit quickly.
void SweepJob::appl_communicate() {
	LOG(V5_DEBG, "SWEEP appl_communicate() \n");
	float t0 = Timer::elapsedSeconds();

	if (_bcast && _is_root && !_terminate_all.load(std::memory_order_relaxed))
		_bcast->updateJobTree(getJobTree());

	//By having rootStartNewSharingRound() before advanceAllReduction(),
	//we dont immediately start a new broadcast after a successful Allreduction extraction which resets the broadcast object
	//this heavily reduces the occurrences of broadcasts in the lower ranks forcing an early extraction of their result because they need to create a new blank _red object
	//at the cost of slightly less frequent sharing rounds. If we wanted maximum sharing frequency, a callback in the allreduction would probably be needed,
	//which would force the extraction immediately after the results arrives in _red...
	rootStartNewSharingRound();
	advanceAllReduction();
	sendWorkstealsViaMPI();

	checkSharingDelay();
	printIdleWorkStatus();
	checkForUnsatResults();
	clearImportedRound();


	float t1 = Timer::elapsedSeconds();
	_duration_appl_communicate.push_back(t1-t0);
	LOG(V5_DEBG, "SWEEP appl_communicate(): %.6f sec \n", (t1-t0));
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

		LOG(V4_VVER, "SWEEP MSG [%i] <---?---- [%i](%i) \n", _my_rank, sourceRank, sourceLocalId);

		if (_terminate_all.load(std::memory_order_relaxed)) {
			LOG(V3_VERB, "SWEEP Not answering to post-termination MPI steal request from [%i](%i) \n", sourceRank, sourceLocalId);
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

		//in case we don't have full tree information about the origin of the message we can still send it back, because we can use metadata in the message itself as a backup
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

		//probably happens when sweep message slips right into the ongoing ranklist update that is periodically started as aggregating, in job.cpp 233
		assert(msg.contextIdOfDestination != 0 ||
			log_return_false("SWEEP STEAL ERROR: invalid contextIdOfDestination==0. In TAG_RETURNING_STEAL_REQUEST, wanted to return an message"
					"With sourceRank=%i, sourceIndex=%i, payload.size()=%zu \n", sourceRank, sourceIndex, msg.payload.size()));

		assert(msg.treeIndexOfDestination >= 0 ||
			log_return_false("SWEEP STEAL ERROR: treeIndexOfDestination < 0 . In TAG_RETURNING_STEAL_REQUEST, wanted to return an message"
					"With sourceRank=%i, sourceIndex=%i, contextIdOfDestination=%i, payload.size()=%zu \n", sourceRank, sourceIndex, msg.contextIdOfDestination, msg.payload.size()));

		if (stolen_count>0) {
			LOG(V4_VVER, "SWEEP snd [%i] >>>>%i>>>> [%i](%i) \n", _my_rank, stolen_count, sourceRank, sourceLocalId);
		}

		getJobTree().send(sourceRank, MSG_SEND_APPLICATION_MESSAGE, msg);
	}
	else if (msg.tag == TAG_RETURNING_STEAL_REQUEST) {
		int stealingLocalId = msg.payload.back();
		msg.payload.pop_back();
		assert(_worksteal_requests[stealingLocalId].got_steal_response == false || log_return_false("SWEEP ERROR : got MPI steal answer, but already request.got_steal_response==true.  sourceRank %i, stealingLocalId %i, payload.size %zu ", sourceRank, stealingLocalId, msg.payload.size()));
		assert(_worksteal_requests[stealingLocalId].to_send == false			|| log_return_false("SWEEP ERROR : got MPI steal answer, but still   request.to_send==true.             sourceRank %i, stealingLocalId %i, payload.size %zu ", sourceRank, stealingLocalId, msg.payload.size()));

		_worksteal_requests[stealingLocalId].stolen_work = std::move(msg.payload);
		_worksteal_requests[stealingLocalId].got_steal_response = true;
		if (_worksteal_requests[stealingLocalId].stolen_work.size() > 0)
			LOG(V4_VVER, "SWEEP rcv [%i](%i) <<<%zu<<<< [%i]\n", _my_rank, stealingLocalId, _worksteal_requests[stealingLocalId].stolen_work.size(), sourceRank );
		else
			LOG(V4_VVER, "SWEEP rcv [%i](%i) <---0---- [%i]\n", _my_rank, stealingLocalId, sourceRank );
	}
	else if (msg.tag == TAG_FOUND_UNSAT) {
		LOG(V2_INFO, "SWEEP MSG [%i] <~~~ Found UNSAT! [%i]\n", _my_rank, sourceRank );
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
	// _external_termination = true;
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
	LOG(V3_VERB, "SWEEP TERM #%i [%i] isDestructible? yes. now joining... \n",  getId(),_my_rank);
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
		LOG(V2_INFO, "SWEEP [%i] (job #%i) sending UNSAT to root \n", _my_rank,getId());
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
		LOG(V2_INFO, "SWEEP JOB [%i]: Solution Size %zu\n", _my_rank, formula.size());
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
	LOG(V4_VVER, "SWEEP JOB [%i](%i) queuing background worker thread\n", _my_rank, localId);
	_bg_workers[localId]->run([this, localId]() {
		LOG(V3_VERB, "SWEEP JOB [%i](%i) WORKER START \n", _my_rank, localId);

		auto sweeper = createNewSweeper(localId);

		loadFormula(sweeper);
		_started_sweepers_count++; //only additive, monotonically increasing, going from 0...nThreads-1 and never decreases
		_running_sweepers_count++; //tracks actual number of running solvers at any given moment in time
		/*
		 *  Syncronization Layer!
		 *  We wait here until all other solvers are also initialized, and only then start solving
		 *  This is relevant for sweeping quality, as otherwise the solvers joining late might miss some of the equalities shared in the first rounds
		 *  Alternatively, store all the shared information as a warmup-greeting-package for newly joining solvers, to maximize quality. But only relevant if the SweepJob grows with time, which is not currently the case.
		 */
		while (_started_sweepers_count < _nThreads) {
			LOG(V5_DEBG, "SWEEP [%i](%i) waits for other solvers (started %i/%i)\n", _my_rank, localId, _started_sweepers_count.load(), _nThreads);
			usleep(2000); //2ms
			if (_terminate_all) {
				break;
			}
		}

		if (_terminate_all) {
			LOG(V3_VERB, "SWEEP [%i](%i): terminated while waiting in synchronization \n", _my_rank, localId);
			_running_sweepers_count--;
			_finished_sweepers_count++; //only monotonically increasing
			_terminated_while_synchronizing = true;
			// maybe this release helps with memory?
			// kissat_release(sweeper->solver);
			return;
		}

		_sweepers[localId] = sweeper; //only now expose the solver to the rest of the system, now that we know we start solving
		_started_synchronized_solving = true; //multiple threads will write non-thread-safe to this bool, but all will write monotonically "true"

		LOG(V3_VERB, "SWEEP [%i](%i) START solve() \n", _my_rank, localId);
		int res = sweeper->solve(0, nullptr);
		LOG(V3_VERB, "SWEEP [%i](%i) FINISH solve(). Result %i \n", _my_rank, localId, res);

		//transfer some solver-specific statistics
		// auto stats = sweeper->fetchSweepStats();
		// _worksweeps[localId] = stats.worksweeps;
		// _resweeps_in[localId] = stats.resweeps_into_work;
		// _resweeps_out[localId] = stats.resweeps_outof_work;

		// if (sweeper->is_congruencer) {
			// printCongruenceStats(sweeper);
		// }

		if (res==UNSAT) {
			//Found UNSAT
			assert(kissat_is_inconsistent(sweeper->solver) || log_return_false("SWEEP ERROR: Solver returned UNSAT 20 but is not in inconsistent (==UNSAT) state!\n"));
			LOG(V2_INFO, "SWEEP [%i](%i) found UNSAT! \n", _my_rank, localId);
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
			reportEndStats(sweeper);
		}

		//If no solver sets UNSAT or IMPROVED, the job will be returned by default as UNKNOWN

		// if (_running_sweepers_count==1) { //the last solver should report resweeps, as only then they are gathered from all exited solvers
			// printResweeps();
		// }

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
	float init_dur =  (t1 - t0);
	const float WARN_init_dur = 0.050; //Usual initializations take 0.2ms in the Sat Solver Subprocess and 4-25ms  in the sweep job (for some weird reasons), but should never be above ~30ms, we warn at >50ms
	LOG(V3_VERB, "SWEEP [%i](%i) kissat init %.6f sec (%i/%i started)\n", _my_rank, localId, init_dur, _started_sweepers_count.load(), _nThreads);
	if (init_dur > WARN_init_dur) {
		LOG(V1_WARN, "SWEEP WARN STARTUP [%i](%i): kissat init took unusually long, %.6f sec !\n", _my_rank, localId, init_dur);
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
		sweeper->sweepSetFormulaReportCallback();
		sweeper->setRepresentativeLocalId(_representative_localId);
		//One representive solver at the root node reports about its new kissat-internal state after each iteration (e.g., number of active variables, clauses, etc)
		if (localId==_representative_localId) {
			shweep_set_report_finished_iteration_callback(sweeper->solver, this, cb_report_iteration);
		}
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
	sweeper->set_option("mallob_individual_sweepiters", _params.sweepIndividualSweepIters.val);

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
  	sweeper->set_option("sweepvars", 256);			//  256,  0, INT_MAX,	"environment variables")
  	sweeper->set_option("sweepmaxvars", 8192);		//	8192, 2, INT_MAX,	"maximum environment variables")
  	sweeper->set_option("sweepdepth", 2);			//, 2,    0, INT_MAX,	"environment depth")
  	sweeper->set_option("sweepmaxdepth", _params.sweepMaxDepth.val); //	//	3,    1, INT_MAX,	"maximum environment depth")
  	sweeper->set_option("sweepclauses", 1024);		//	1024, 0, INT_MAX,	"environment clauses")
  	sweeper->set_option("sweepmaxclauses", 32768);	//	32768,2, INT_MAX,	"maximum environment clauses")
  	sweeper->set_option("sweepfliprounds", 1);		//	1,    0, INT_MAX,	"flipping rounds")
  	sweeper->set_option("sweeprand", 0);			//  0,    0,    1,		"randomize sweeping environment")

	sweeper->set_option("substitute", 1);			// (default 1) apply equivalence substitutions after sweeping, keep here explicitly to remember it
	sweeper->set_option("substituterounds", 2);		// (default 2) there does not seem to be any need to go higher, as almost always all equivalences are already found in the very first round


	sweeper->set_option("preprocess", 0); //to skip this part (other preprocessing stuff after shweep finished)
	sweeper->set_option("luckyearly", 0); //to skip this part
	sweeper->set_option("luckylate", 0);  //to skip this part
	sweeper->interruptionInitialized = true;
	return sweeper;
}

void SweepJob::cbReportIteration(int localId) {
	assert(_is_root || log_return_false("SWEEP ERROR : iteration report in a non-root rank. Technically possible, but currently not allowed \n"));
	assert(localId == _representative_localId);
	KissatPtr sweeper = _sweepers[localId];
	assert(sweeper);
	// int iteration = shweep_get_curr_iteration(sweeper->solver);
	auto stats = sweeper->fetchSweepStats();

	LOG(			  V2_INFO, "SWEEP solver [%i](%i) reports statistics in dedicated .sweep file \n", _my_rank, localId);
	LOGGER(_reslogger, V2_INFO, "\n");
	LOGGER(_reslogger,V2_INFO, "Reported by [%i](%i)		\n", _my_rank, localId);
	LOGGER(_reslogger,V2_INFO, "ITERATION_CURR    %i		\n", stats.curr_iteration);
	LOGGER(_reslogger,V2_INFO, "ITERATIONS_MAX     %i		\n", _params.sweepMaxIterations());
	LOGGER(_reslogger,V2_INFO, "TIME                    %.3f s\n", Timer::elapsedSeconds() - _start_sweep_timestamp);
	LOGGER(_reslogger,V2_INFO, "ACTIVE_PRCNT            %.2f %\n", 100*(double)stats.curr_active/(double)stats.vars_active_orig);
	LOGGER(_reslogger,V2_INFO, "ENV_LIMIT_VARS    %i 		\n", stats.env_limit_vars);
	LOGGER(_reslogger,V2_INFO, "ENV_LIMIT_DEPTH   %i 		\n", stats.env_limit_depth);
	LOGGER(_reslogger,V2_INFO, "ENV_LIMIT_CLAUSES %i 		\n", stats.env_limit_clauses);
	LOGGER(_reslogger,V2_INFO, "ROUNDS_THISITER     %i   	\n",_root_rounds_this_iteration);
	LOGGER(_reslogger,V2_INFO, "EMPTYROUNDS_BP      %i   	\n",_root_emptyrounds_before_progress);

	LOGGER(_reslogger,V2_INFO, "CLAUSES_CURR		%i 		\n", stats.clauses);
	LOGGER(_reslogger,V2_INFO, "CLAUSES_ORIG		%i 		\n", stats.clauses_orig);
	LOGGER(_reslogger,V2_INFO, "BINIRR_CURR				%i	\n", stats.binirr);
	LOGGER(_reslogger,V2_INFO, "BINIRR_ORIG				%i	\n", stats.binirr_orig);
	LOGGER(_reslogger,V2_INFO, "ACTIVE_CURR      %i			\n", stats.curr_active);
	LOGGER(_reslogger,V2_INFO, "ACTIVE_ORIG      %i			\n", stats.vars_active_orig);
	LOGGER(_reslogger,V2_INFO, "NONACTIVE_CURR          %i	\n", stats.vars_active_orig - stats.curr_active);
	LOGGER(_reslogger,V2_INFO, "ELIMINATED       %i 		\n", stats.curr_eliminated);
	LOGGER(_reslogger,V2_INFO, "NEWUNITS         %i 		\n", stats.curr_units - stats.units_orig);
	LOGGER(_reslogger,V2_INFO, "ALLUNITS         %i 		\n", stats.curr_units);
	LOGGER(_reslogger,V2_INFO, "SUM_ELIM_NEWU           %i	\n", stats.curr_eliminated + stats.curr_units - stats.units_orig );
	LOGGER(_reslogger,V2_INFO, "SWEEPUNITS       %i 		\n", stats.sweep_units);
	LOGGER(_reslogger,V2_INFO, "EQUIVALENCES     %i 		\n", stats.sweep_eqs);
	LOGGER(_reslogger,V2_INFO, "SUM_SWEEP_EU            %i  \n", stats.sweep_eqs + stats.sweep_units);
	LOGGER(_reslogger,V2_INFO, "\n");
}



void SweepJob::reportEndStats(KissatPtr sweeper) {
	assert(_is_root);
	assert(sweeper->getLocalId() == _representative_localId);

	LOGGER(_reslogger,V2_INFO, "SWEEP_PRIORITY       %.3f\n", _params.preprocessSweepPriority.val);
	LOGGER(_reslogger,V2_INFO, "SWEEP_PROCESSES      %i\n",  getVolume());
	LOGGER(_reslogger,V2_INFO, "SWEEP_THREADS_PER_P  %i\n", _nThreads);
	LOGGER(_reslogger,V2_INFO, "SWEEP_SHARING_PERIOD_PARAM   %.3f s \n", _params.sweepSharingPeriod.val);


	static const int DURATION_WARN_FACTOR=2;
	if (_timestamp_root_started_bcast.size()>1) {
		float total_sharing_time = _timestamp_root_started_bcast.back() - _timestamp_root_started_bcast.front();
		float avg_sharing_period = total_sharing_time / (_timestamp_root_started_bcast.size()-1);
		LOGGER(_reslogger,V2_INFO, "SWEEP_SHARING_PERIOD_AVG     %.3f s \n", avg_sharing_period);
		for (int i=0; i < _timestamp_root_started_bcast.size()-1; i++) {
			float period = _timestamp_root_started_bcast[i+1] - _timestamp_root_started_bcast[i];
			if (period > DURATION_WARN_FACTOR*avg_sharing_period) {
				LOGGER(_reslogger,V1_WARN, "[WARN] SWEEP_SHARING_PERIOD_REAL %.3f sec   (round %i) is much larger than average %.4f sec \n", period, i, avg_sharing_period);
			}
		}
	}

	float max_appl_comm_duration = *std::max_element(_duration_appl_communicate.begin(), _duration_appl_communicate.end());
	LOGGER(_reslogger,V2_INFO, "SWEEP_APPL_COMMUNICATE_MAX   %.6f s \n", max_appl_comm_duration);

	for (int i=0; i<15 && i<_internal_result.getSolutionSize(); i++) {
		LOGGER(_reslogger,V3_VERB, "RESULT Sweep Formula[%i] = %i \n", i, _internal_result.getSolution(i));
	}
}

// void SweepJob::printCongruenceStats(KissatPtr sweeper) {
	// auto stats = sweeper->fetchSweepStats();
	// LOGGER(_reslogger, V2_INFO, "CONGRUENCE_EQUIVALENCES   %i \n", stats.congr_eqs);
	// LOGGER(_reslogger, V2_INFO, "CONGRUENCE_UNITS          %i \n", stats.congr_units);
	// LOGGER(_reslogger, V2_INFO, "CONGRUENCE_EQS_SKIPPED    %i / %i \n", stats.congr_eqs_skipped, stats.eqs_seen);
// }


void SweepJob::printIdleWorkStatus() {
	if (_terminate_all.load(std::memory_order_relaxed))
		return; //prevent segfault! when termination is triggered, the sweeper references might suddenly become invalid. no touching them

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
				sweeper->sweeper_longterm_idle = true; //those solvers which are idle right now are candicates for being also longterm idle
			}
			oss_work << shweep_get_work_estimate(sweeper->solver) << ",";
		} else {
			oss_work << "--,";
		}
	}
	_lastLongtermIdleCount = longterm_idles;
	LOG(V3_VERB, "SWEEP [%i]                          rng %i   idle(long) %i(%i) %s   Work[%i]: %s\n", _my_rank, _running_sweepers_count.load(), idles, longterm_idles, oss_idles.str().c_str(), _my_rank, oss_work.str().c_str());
}

void SweepJob::checkSharingDelay() {
	if (_terminate_all.load(std::memory_order_relaxed))
		return;


	//We insert manually a first timestamp to detect cases where NO communication happened at all for a specific rank (due to a programming bug in the job tree handling)
	//With the manual insertions we have a reference against which delays can be detected, and can be sure that we are warned if this rank does not participate in the overall communication. i
	//Otherwise the timing-vectors could have remained completely empty and no delay-difference would be detected
	// if (!_started_sharedelay_tracking && _started_synchronized_solving) {
		// float t = Timer::elapsedSeconds();
		// _timestamp_contributed_to_sharing.push_back(t);
		// _timestamp_receive_sharing_result.push_back(t);
		// _started_sharedelay_tracking = true;
	// }

	constexpr float MAX_DELAY_FACTOR = 6;
	float time = Timer::elapsedSeconds();

	float expected_period = _params.sweepSharingPeriod.val; //in seconds
	if (!_timestamp_contributed_to_sharing.empty()) {
		float delay = time - _timestamp_contributed_to_sharing.back();
		if (delay > expected_period*MAX_DELAY_FACTOR) {
			//We log two times. Once in the main log file to see the information chronologically correct interleaved with the other logs
			//and onces separately in a .warn file for faster grepping during post-processing, where we would like to avoid to grep through the main logs
			LOG(				V1_WARN, "WARN SWEEP SHARINGDELAY [%i]: %.2f sec since contrib, factor %.1f \n", _my_rank, delay, delay/expected_period);
			LOGGER(_warnlogger, V1_WARN, "WARN SWEEP SHARINGDELAY [%i]: %.2f sec since contrib, factor %.1f \n", _my_rank, delay, delay/expected_period);
		}
	}
	if (!_timestamp_receive_sharing_result.empty()) {
		float delay = time - _timestamp_receive_sharing_result.back();
		if (delay > expected_period*MAX_DELAY_FACTOR) {
			LOG(			    V1_WARN, "WARN SWEEP SHARINGDELAY [%i]: %.2f sec since recv, factor %.1f \n", _my_rank, delay, delay/expected_period);
			LOGGER(_warnlogger, V1_WARN, "WARN SWEEP SHARINGDELAY [%i]: %.2f sec since recv, factor %.1f \n", _my_rank, delay, delay/expected_period);
		}
	}
}

bool SweepJob::okToTrackSharingDelay() {
	return true; //with always advance it should now no longer be an issue to immediately track any sharings...

	//we would get false positives if we already start timing when the solvers haven't even started yet
	// if (!_started_synchronized_solving)
		// return false;
	//we also would get false positive if the solvers have technically started, but haven't received any initial work yet
	//this second consideration only works at the root node, the other nodes don't have such a live update whether the work for this iteration has been provided yet
	// if (_is_root && !_root_provided_initial_work)
		// return false;
	// return true;
}

// void SweepJob::printResweeps() {
	// std::ostringstream oss;
	// int worksweeps = 0;
	// int resweeps_in = 0;
	// int resweeps_out = 0;
	// for (int i=0; i<_nThreads; i++) {
		// oss << " (id=" << i
		// <<" ws=" << _worksweeps[i]
		// <<" rsi=" << _resweeps_in[i]
		// <<" rso=" << _resweeps_out[i]
		// <<") ";
		// worksweeps += _worksweeps[i];
		// resweeps_in += _resweeps_in[i];
		// resweeps_out += _resweeps_out[i];
	// }
	// LOG(V3_VERB, "SWEEP WORKSWEEPS,RESWEEPS: %s \n", _my_rank, oss.str().c_str()); //information for each individual thread
	// LOGGER(_reslogger, V2_INFO, "[%i] SWEEP_WORKSWEEPS   %i \n", _my_rank, worksweeps);
	// LOGGER(_reslogger, V2_INFO, "[%i] SWEEP_RESWEEPS_ALL %i \n", _my_rank, resweeps_in + resweeps_out);
	// LOGGER(_reslogger, V3_VERB, "[%i] SWEEP_RESWEEPS_IN  %i \n", _my_rank, resweeps_in);
	// LOGGER(_reslogger, V3_VERB, "[%i] SWEEP_RESWEEPS_OUT %i \n", _my_rank, resweeps_out);
// }


bool SweepJob::skip_MPI_forNow() {
	return (getJobComm().size() < getVolume());
	// LOG(V4_VVER, "SWEEP [%i] Skip MPI workstealing, jobcomm size %i < volume %i\n", _my_rank, getJobComm().size(), getVolume());
}

void SweepJob::sendWorkstealsViaMPI() {
	if (_terminate_all.load(std::memory_order_relaxed))
		return;

	//Worksteal requests need to be sent by the MPI *main* thread. If kissat-threads themselves send MPI messages, things can crash, since they clash somehow with the MPI hierarchy
	//So each solver-thread queues a steal-request to shared memory, where the main MPI thread can pick it up (here) and send an MPI msg on behalf of the solver
	for (auto &request : _worksteal_requests) {
		if (request.to_send) {
			int senderLocalId = request.senderLocalId;

			//if we are still in the phase where MPI sends are not done, we short-fuse the requests to a zero dummy and return them to the solver threads
			//same if the whole job is terminated
			// if (skip_MPI_forNow() || _terminate_all || getVolume()==0) {
			if (skip_MPI_forNow()) {
				// request.stolen_work = {}; //remains empty from request initialization
				request.to_send = false;
				request.got_steal_response = true;
				LOG(V3_VERB, "SWEEP [%i] mainthread SKIP MPI-steal request by (%i) \n", _my_rank, senderLocalId);
				continue;
			}

			//There was no local work available, now we prepare to send out an MPI message
			int my_comm_rank = getJobComm().getWorldRankOrMinusOne(_my_index);
			if (my_comm_rank == -1) {
				LOG(V3_VERB, "SWEEP SKIP own rank [%i] (myindex %i) <ctx %i> not yet in JobComm of size %zu \n", _my_rank, _my_index, _my_ctx_id, getJobComm().size());
				continue;
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
					LOG(V3_VERB, "SWEEP SKIP target idx %i not in JobComm (size %zu) \n", targetIndex, getJobComm().size());
					continue;
				}
				if (getJobComm().getContextIdOrZero(targetIndex)==0) {
					//target is not yet listed in address list. Might happen for a short period just after it is spawned. roll again
					LOG(V3_VERB, "SWEEP SKIP ctx_id of target is missing. getVolume()=%i, rndTargetIndex=%i, rndTargetRank=%i, myIndex=%i, myRank=%i, JobComm size %zu \n", getVolume(), targetIndex, targetRank, _my_index, _my_rank, getJobComm().size());
					continue;
				}
				foundRank = targetRank;
				foundIndex = targetIndex;
				// request.targetIndex = targetIndex;
				// request.targetRank = targetRank;
				break;
			}

			if (foundIndex==-1 || foundRank==-1) {
				//couldn't find a target for this request, skip it for now and process the next
				LOG(V4_VVER, "SWEEP MSG [%i](%i) SKIP ------ no target possible yet  \n", _my_rank, request.senderLocalId);
				continue;
			}

			request.targetIndex = foundIndex;
			request.targetRank = foundRank;

			assert(request.targetIndex>=0 || log_return_false("SWEEP ERROR: request.targetIndex %i \n", request.targetIndex));
			assert(request.targetRank>=0  || log_return_false("SWEEP ERROR: request.targetRank %i \n", request.targetIndex));

			// request.sent = true;
			JobMessage msg = getMessageTemplate();
			msg.tag = TAG_SEARCHING_WORK;
			//Need to add these two fields because we are doing arbitrary point-to-point communication
			msg.treeIndexOfDestination = request.targetIndex;
			msg.contextIdOfDestination = getJobComm().getContextIdOrZero(request.targetIndex);
			assert(msg.contextIdOfDestination != 0 || log_return_false("SWEEP ERROR: contextIdOfDestination==0 in workstealing request! Source rank=%i, targetRank %i \n", _my_rank, request.targetRank));
			assert(msg.treeIndexOfDestination >= 0 || log_return_false("SWEEP ERROR: treeIndexOfDestination < 0 in workstealing request! Source rank=%i, targetRank %i \n", _my_rank, request.targetRank));
			//We also send our contextId. Because it can happen that the receiving rank does not yet know our contextId,
			//which (probably) happens when a worksteal request is sent right when also the ranklist aggregation update is propagating through all ranks, where some (like this rank here) are already updated, while others (like the receiving rank) aren't.
			//So as a backup for the receiving rank, we also provide our contextId
			int myContexId = getJobComm().getContextIdOrZero(_my_index);
			//Update: and we also send our treeIndex for the same reason, the receiver might not yet have our address data
			msg.payload = {request.senderLocalId, myContexId, _my_index};
			assert(msg.payload.size() == NUM_SEARCHING_WORK_FIELDS);
			LOG(V4_VVER, "SWEEP MSG [%i](%i) ---?---> [%i] \n", _my_rank, request.senderLocalId, request.targetRank);
			getJobTree().send(request.targetRank, MSG_SEND_APPLICATION_MESSAGE, msg);
			//CRUCIAL that this flag is only set now, after we did everything with it.
			//because after resetting this flag, a workstealing solver could immediately concurrently modify this request, by resetting it
			request.to_send = false;
		}
	}
}


//For development purposes: A simple interface to communicate some integers between solver and Mallob without the need to declare dedicated new functions each time
int SweepJob::cbCustomQuery(int query) {
	if (query==QUERY_SWEEP_ITERATION) {
		return _root_sweep_iteration;
	}

	return 0;
}



void SweepJob::checkForNewImportRound(KissatPtr sweeper) {
	int available_import_round = _available_import_round.load(std::memory_order_acquire);
	int my_last_import_round = sweeper->sweep_import_round;
	if (available_import_round != my_last_import_round) [[unlikely]] {
		//there is new data from a new sharing round
		// publish_round = _sharing_import_round.load(std::memory_order_acquire);
		LOG(V4_VVER, "SWEEP [%i](%i) see round %i --> %i \n", _my_rank, sweeper->getLocalId(), my_last_import_round, available_import_round);

		assert(my_last_import_round <= available_import_round);
		if (my_last_import_round!=0 && my_last_import_round != available_import_round - 1) {
			LOG(V1_WARN, "SWEEP WARN SKIP: Solver [%i](%i) skipped import rounds, went %i -> %i \n", _my_rank, sweeper->getLocalId(), my_last_import_round, available_import_round);
		}
		sweeper->sweep_import_round  = available_import_round;
		if (sweeper->sweep_EQS_index != sweeper->sweep_EQS_size)
			LOG(V1_WARN, "SWEEP WARN SKIP: Solver [%i](%i) couldn't finish reading previous eqs  imports (of round %i)! now skipping remaining  %i/%i \n", _my_rank, sweeper->getLocalId(), my_last_import_round, sweeper->sweep_EQS_index.load(), sweeper->sweep_EQS_size.load());
		if (sweeper->sweep_UNITS_index != sweeper->sweep_UNITS_size)
			LOG(V1_WARN, "SWEEP WARN SKIP: Solver [%i](%i) couldn't finish reading previous unit imports (of round %i)! now skipping remaining  %i/%i \n", _my_rank, sweeper->getLocalId(), my_last_import_round, sweeper->sweep_UNITS_index.load(), sweeper->sweep_UNITS_size.load());
		//tell the solver where in the fixed array the data starts and where it ends
		sweeper->sweep_EQS_index   = 0;
		sweeper->sweep_UNITS_index = 0;
		sweeper->sweep_EQS_size    = _EQS_import_size.load(std::memory_order_relaxed);
		sweeper->sweep_UNITS_size  = _UNITS_import_size.load(std::memory_order_relaxed);
		// LOG(V2_INFO, "Solver [%i](%i) updates to new round %i with eq_size %i, unit_size %i \n", _my_rank, sweeper->getLocalId(), publish_round, sweeper->sweep_EQS_size.load(), sweeper->sweep_UNITS_size.load());
	}
}

#define SWEEP_NEW_IMPORT_VERSION 1

void SweepJob::cbImportEq(int *ilit1, int *ilit2, int localId) {
	// if (_terminate_all) { //can happen that we arrive here before the solver learned that we already terminated
		//leave *ilit's untouched
		// return;
	// }

	KissatPtr sweeper = _sweepers[localId];

#if SWEEP_NEW_IMPORT_VERSION == 0

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

	assert(sweeper->sweep_EQS_index <= sweeper->sweep_EQS_size	|| log_return_false("SWEEP ERROR: in Equivalence Import: index %i is now beyond size %i \n", sweeper->sweep_EQS_index.load(), sweeper->sweep_EQS_size.load()));
	assert(*ilit1 !=0 || *ilit2 !=0								|| log_return_false("SWEEP ERROR: in cbImportEq: sending invalid empty *ilit1=%i, *ilit2=0 to the solvers\n", *ilit1, *ilit2));
	assert(*ilit1 < *ilit2										|| log_return_false("SWEEP ERROR: in cbImportEq: *ilit1 %i is larger than %i *ilit2, but they should be sorted (index %i, %i,)\n", *ilit1, *ilit2, idx, idx+1));

#else

	const int idx   = sweeper->curr_eq_index;
	const int round = sweeper->curr_eq_round;
	std::vector<int> &eqs = _imported_EQS_UNITS[round].eqs;
	//Try to (continue) importing from the round we are currently reading from
	if (idx < eqs.size()) {
		*ilit1 = eqs[idx];
		*ilit2 = eqs[idx+1];
		sweeper->curr_eq_index+=2;
		assert(*ilit1 !=0 || *ilit2 !=0		|| log_return_false("SWEEP ERROR: in cbImportEq: sending invalid empty *ilit1=%i, *ilit2=0 to the solvers\n", *ilit1, *ilit2));
		assert(*ilit1 < *ilit2				|| log_return_false("SWEEP ERROR: in cbImportEq: *ilit1 %i is larger than %i *ilit2, but they should be sorted (index %i, %i,)\n", *ilit1, *ilit2, idx, idx+1));
		if (sweeper->curr_eq_index == eqs.size()) {
			LOG(V5_DEBG, "SWEEP [%i](%i) ((( < %i > E %i \n", _my_rank, sweeper->getLocalId(),  round, eqs.size()/2);
		}
	}
	//Check if there is a next round to import
	else if (round < _lastImportedRound) {
		if (eqs.size()==0) { //for completeness we also log the edge-case where there was nothing to import
			LOG(V5_DEBG, "SWEEP [%i](%i) ((( < %i > E %i \n", _my_rank, sweeper->getLocalId(), round,  eqs.size()/2);
		}
		//Advance to import from the next stored round data. This does not necessarily need to be the latest round, if there are be multiple rounds stored we might need to catch up sequentially through all of them
		//Keep track how many threads have finished reading this round, such that it can be deleted after all threads have read it
		_finishedRoundCounters[round].threads_finished_eqs++;
		sweeper->curr_eq_round++;
		sweeper->curr_eq_index=0;
	}

#endif
	//now returning to the kissat solver
}


void SweepJob::cbImportUnit(int *ilit, int localId) {
	// if (_terminate_all) {
		// return;
	// }
	KissatPtr sweeper = _sweepers[localId];


#if SWEEP_NEW_IMPORT_VERSION == 0

	checkForNewImportRound(sweeper);
	if (sweeper->sweep_UNITS_index == sweeper->sweep_UNITS_size) {
		// leave *ilit untouched
		return;
	}
	assert(sweeper->sweep_UNITS_index < sweeper->sweep_UNITS_size || log_return_false("SWEEP ERROR: in Unit Import: curr index %i is larger than expected size %i\n", sweeper->sweep_UNITS_index.load(), sweeper->sweep_UNITS_size.load()));
	int idx = sweeper->sweep_UNITS_index.load();
	*ilit = _UNITS_to_import[idx];
	sweeper->sweep_UNITS_index++;
	assert(sweeper->sweep_UNITS_index <= sweeper->sweep_UNITS_size || log_return_false("SWEEP ERROR: in Unit Import: index %i is now beyond size %i \n", sweeper->sweep_UNITS_index.load(), sweeper->sweep_UNITS_size.load()));

#else
	//For comments see cbImportEq (the analog method for importing equalities)
	const int idx   = sweeper->curr_unit_index;
	const int round = sweeper->curr_unit_round;
	std::vector<int> &units = _imported_EQS_UNITS[round].units;
	if (idx < units.size()) {
		*ilit = units[idx];
		sweeper->curr_unit_index++;
		if (sweeper->curr_unit_index == units.size()) {
			LOG(V5_DEBG, "SWEEP [%i](%i) ((( < %i > U %i \n", _my_rank, sweeper->getLocalId(), round, units.size() );
		}
	}
	else if (round < _lastImportedRound) {
		if (units.size()==0) {
			LOG(V5_DEBG, "SWEEP [%i](%i) ((( < %i > U %i \n", _my_rank, sweeper->getLocalId(), round, units.size());
		}
		_finishedRoundCounters[round].threads_finished_units++;
		sweeper->curr_unit_round++;
		sweeper->curr_unit_index=0;
	}
#endif

	//now returning to kissat solver
}

bool SweepJob::tryProvideInitialWork(KissatPtr sweeper) {
		//we only provide work at the root node, this simplifies its tracking, and especially allows the root-transform to know and influence the work-sharing state via shared-memory
	if (_is_root
		//to avoid any concurrency problems, only a single hardcoded representative solver (localId 0 per default) receives the work
		&& sweeper->getLocalId()==_representative_localId
		//to prevent that we provide work multiple times in the same iteration, check explicitly that the solver expects it for a new iteration
		&& shweep_get_curr_iteration(sweeper->solver) > _root_sweep_iteration)
	{

		sweeper->sweeper_is_idle = false; //already set non-idle here to prevent case where solver is already initialized, non-idle, but still has no work cause its just being copied, and then a sharing operation starts right now, terminating everything wrongly early
		sweeper->sweeper_longterm_idle = false;

		//We need to know how much space to allocate to store each variable "idx" at the array position work[idx], i.e. we need to know max(idx).
		//We assume that the maximum variable index corresponds to the total number of variables
		//i.e. we assume that there are no holes in kissats internal numbering. This is an assumption that standard Kissat makes all the time, so we also do it here

		const unsigned VARS = shweep_get_num_vars(sweeper->solver); //this value can be different from numVars here in C++ !! Because kissat might have aready propagated some units, etc.
		LOG(V2_INFO, "SWEEP WORK PROVIDING --------------%u---------------- \n", VARS);
		sweeper->work_received_from_steal = std::vector<int>(VARS);
		_root_initwork_startedproviding = true;

		//the initial work is all variables
		for (int idx = 0; idx < VARS; idx++) {
			sweeper->work_received_from_steal[idx] = idx;
		}
		LOG(V2_INFO, "SWEEP WORK PROVIDED  --------------%u----------------> to sweeper [%i](%i)\n", VARS, _my_rank, sweeper->getLocalId());
		_root_initwork_provided = true;
		return true;
	}

	return false;

}

void SweepJob::solverGoStealing(KissatPtr sweeper) {
	usleep(1000);
	int localId = sweeper->getLocalId();
	sweeper->work_received_from_steal = {};

	// LOG(V3_VERB, "Sweeper [%i](%i) stealing \n", _my_rank, localId);

	if (_terminate_all.load(std::memory_order_relaxed)) {
		sweeper->sweeper_is_idle = true;
		LOG(V3_VERB, "Sweeper [%i](%i) exit mallob steal due to terminate_all\n", _my_rank, localId);
		//just to be safe, send another termination to self
		sweeper->triggerSweepTerminate(_params.sweepIndividualSweepIters.val);
		sweeper->count_repeated_missed_termination++;
		if (sweeper->count_repeated_missed_termination % sweeper->WARN_ON_REPEATED_MISSED_TERMINATION==0) {
			LOG(V1_WARN, "SWEEP WARN : Sweeper [%i](%i) in %i-th worksteal loop after termination\n", _my_rank, localId, sweeper->count_repeated_missed_termination);
		}
		return;
	}

	if (shweep_get_end_iteration_signal(sweeper->solver)) {
		LOG(V3_VERB, "Sweeper [%i](%i) exit mallob steal due to end_iteration flag \n", _my_rank, localId);
		return;
	}

	if (tryProvideInitialWork(sweeper)) {
		return;
	}



	sweeper->sweeper_is_idle = true;

	//Check whether a previously queued MPI request has been answered successfully
	if (_worksteal_requests[localId].got_steal_response) {
		if (_worksteal_requests[localId].stolen_work.size()>0) {
			sweeper->work_received_from_steal = std::move(_worksteal_requests[localId].stolen_work);
			LOG(V4_VVER, "SWEEP recv [%i](%i) <==%zu==== [%i] \n",  _my_rank, localId, (int)sweeper->work_received_from_steal.size(), _worksteal_requests[localId].targetRank);
			return;
		}
		_worksteal_requests[localId].got_steal_response = false; //to no read it a second time
		_worksteal_requests[localId].is_inactive = true; //request was fully processed, the slot is now effectively empty
	}

	//No success via MPI (either there was no answer, or the answer hat no work).
	//Next try: steal locally
	auto stolen_work = stealWorkFromAnyLocalSolver(_my_rank, localId);
	if ( ! stolen_work.empty()) {
		//Successful local steal
		sweeper->work_received_from_steal = std::move(stolen_work);
		LOG(V4_VVER, "SWEEP lcl [%i](%i) <==%zu====  \n", _my_rank, localId, sweeper->work_received_from_steal.size(), _my_rank);
		return;
	}

	//Did not find work locally. Deposit an MPI request in case there isn't one yet.
	//We deposit a request for an MPI message via shared memory and the main MPI thread of this process will pick up the request and actually send it via MPI
	//This extra step is necessary, because the thread is just "some" solver-thread and it can cause problems it it start sending MPI messages on it's own
	if (_worksteal_requests[localId].is_inactive) {
		//careful, this is close to creating a race-condition with the main-thread, if it just so happens to also read this request right now
		//to prevent the race-condition, the main-thread sets .wait_for_send only to false after it already send out the msg. i.e. this code here can no longer corrupt the outwards send
		_worksteal_requests[localId].newQueuedRequest(localId);
	}

	//If we make it until here we are waiting for work and have have nothing else to do for now. Can wait for  ~1 millisecond until we check the system again.
	usleep(1000);

}

void SweepJob::cbStealWorkNew(unsigned **work, int *work_size, int localId) {
	KissatPtr sweeper = _sweepers[localId]; //this array access is safe because the callback is called by this sweeper itself
	solverGoStealing(sweeper);
	//We store the steal data persistently in the C++ vector sweeper->work_received_from_steal, allocated and managed in C++
	//The kissat solver (and other solver threads) will then be allowed to read and write(!) within this fixed allocated memory.
	*work = reinterpret_cast<unsigned int*>(sweeper->work_received_from_steal.data());
	*work_size = (int)sweeper->work_received_from_steal.size();
	if (*work_size != sweeper->work_received_from_steal.size()) {
		LOG(V1_WARN, "SWEEP WARN ERROR : weird work discrepancy: *work_size==%i, work.size()==%zu \n", *work_size, sweeper->work_received_from_steal.size());
	}
	assert(*work_size>=0 || log_return_false("SWEEP ERROR : work size %i \n", *work_size));
	if (*work_size>0) {
		sweeper->sweeper_is_idle = false;
		sweeper->sweeper_longterm_idle = false;
	}
	//callback ends, kissat thread returns back to its C solver code
}

void SweepJob::rootStartNewSharingRound() {
	if (!_is_root)
		return;
	if (_terminate_all.load(std::memory_order_relaxed))
		return;

	assert(_is_root); //only the root node initiates sharing rounds

	if (!_bcast) {
		LOG(V1_WARN, "SWEEP WARN : SHARE BCAST root couldn't initiate sharing round, _bcast is Null\n");
		return;
	}

	if (_timestamp_root_started_bcast.size()>0 &&  Timer::elapsedSeconds() < _timestamp_root_started_bcast.back() + _params.sweepSharingPeriod.val) {
		//not yet time for next sharing round
		return;
	}

	if (!_started_synchronized_solving.load(std::memory_order_relaxed)) {
		LOG(V3_VERB, "SWEEP root: Delay first round, not all solvers online yet (%i/%i) \n", _started_sweepers_count.load(), _nThreads);
		return;
	}

	if (!_root_initwork_startedproviding) {
		LOG(V3_VERB, "SWEEP root: Wait with next round, haven't started yet to provide new initial work\n");
		return;
		//Waiting until _starting_ to provide initial work seems the sweet-spot in terms of waiting
		//If we didn't check for initial work at all, then it can happen that we have multiple sharing rounds after an iteration ends, which can share all_idle multiple times and could lead to skipped iterations
		//If we waited longer, for example until the work was actually provided, then for large instance we would wait here for quite some time and other ranks would start issuing Sharingdelay warnings
	}

	//make sure that only one sharing operation is going on at a time
	//on this root node, hasReceivedBroadcast is equivalent to asking whether this _bcast object has already started a broadcast
	if (_bcast->hasReceivedBroadcast()) {
		LOG(V4_VVER, "SWEEP root: Delay next round. round %i still ongoing\n", _root_sharing_round);
		return;
	}
	//Broadcast a ping to all workers to initiate an AllReduce
	//The broadcast includes all workers currently reachable by the root-node and informs them about their parent and potential children
	//It then causes the leaf nodes to call the callback, initiating the AllReduce
	// _root_last_sharing_start_timestamp = Timer::elapsedSeconds();
	_timestamp_root_started_bcast.push_back(Timer::elapsedSeconds());
	LOG(V4_VVER, "SWEEP root: Initiating new sharing round via modular broadcast\n");
	JobMessage msg = getMessageTemplate();
	msg.tag = _bcast->getMessageTag();
	msg.payload = {};
	_bcast->broadcast(std::move(msg));
}

void SweepJob::appendMetadataToReductionElement(std::vector<int> &contrib, int is_idle, int unit_size, int eq_size, int work_sweeps, int work_stepovers, int unsched_resweeps) {
	contrib.insert(contrib.end(), NUM_METADATA_FIELDS, 0); //Make space for the upcoming metadata, initialized with zero
	int size = contrib.size();
	int n=0;
	n++; contrib[size - METADATA_TERMINATE]      = 0;  //dummy, for completeness and robustness, to have the n++ count match
	n++; contrib[size - METADATA_SWEEP_ITERATION]= 0;  //dummy, ""
	n++; contrib[size - METADATA_SHARING_ROUND]  = 0;  //dummy, ""
	n++; contrib[size - METADATA_IDLE]       = is_idle;
	n++; contrib[size - METADATA_UNIT_SIZE]  = unit_size;
	n++; contrib[size - METADATA_EQ_SIZE]    = eq_size;
	n++; contrib[size - METADATA_WORK_SWEEPS]     = work_sweeps;
	n++; contrib[size - METADATA_WORK_STEPOVERS]  = work_stepovers;
	n++; contrib[size - METADATA_UNSCHED_RESWEEPS]= unsched_resweeps;
	assert(n==NUM_METADATA_FIELDS || log_return_false("SWEEP ERROR: Added metadata count (%i) doesnt match expected number (%i) \n", n, NUM_METADATA_FIELDS));
}

void SweepJob::cbContributeToAllReduce() {
	assert(_bcast);
	assert(_bcast->hasResult());
	//bcast hasResult present means that this Process got responses from all its children, so the tree structure is correctly known, and we can continue with a contribution and reduction

	// LOG(V4_VVER, "SWEEP BCAST Callback to AllReduce\n");
	auto snapshot = _bcast->getJobTreeSnapshot();

	LOG(V4_VVER, "SWEEP [%i] BCAST complete, callback creating RED & contributing (%i)children: (%i)[%i] , (%i)[%i]  \n",
		_my_rank, snapshot.nbChildren, snapshot.leftChildIndex, snapshot.leftChildNodeRank, snapshot.rightChildIndex, snapshot.rightChildNodeRank);

	if (! _is_root) {
		LOG(V4_VVER, "SWEEP [%i] BCAST RESET non-root \n", _my_rank);
		//Prepare all non-root processes to be ready to receive the next broadcast
		//CRUCIAL:
		//	getJobTree().getSnapshot()
		//		instead of
		//	snapshot !!
		//
		//	Only via getJobTree().. we get the most up-to-date tree, as updated in appl_communicate
		//  Instead with our own snapshot object, we would just re-use the one from last round, i.e. re-use the one from the very first round and never get any updates in, and remain stuck tree-wise
		_bcast.reset(new JobTreeBroadcast(getId(), getJobTree().getSnapshot(), [this]() {cbContributeToAllReduce();}, TAG_BCAST_INIT));
		// _bcast.reset(new JobTreeBroadcast(getId(), snapshot, [this]() {cbContributeToAllReduce();}, TAG_BCAST_INIT)); <<-- remains stuck at the very initial tree snapshot !
		if (getJobTree().getCommSize() < getVolume()) {
			LOG(V1_WARN, ">>>> WARN SWEEP [%i] BCAST Tree size %i smaller than volume %i \n", _my_rank, getJobTree().getCommSize(), getVolume());
		}
	}

	if (_terminate_all.load(std::memory_order_relaxed)) {
		LOG(V4_VVER, "SWEEP BCAST SKIP reduction, status is already _terminate_all\n");
		return;
	}


	if (_red && _red->hasResult()) {
		LOG(V1_WARN, ">>>> Warn SWEEP [%i] Noticing unextracted _red results late during broadcast callback\n", _my_rank);
		extractAllReductionResult();
	}

	JobMessage baseMsg = getMessageTemplate();
	baseMsg.tag = TAG_ALLRED;
	LOG(V4_VVER, "SWEEP [%i] RED SHARE RESET\n", _my_rank);
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
		assert(eq_size%2==0 || log_return_false("ERROR in AGGR: Non-even number %i of equivalence literals, should always come in pairs", eq_size));
		LOG(V5_DEBG, "SWEEP SHARE REDUCE (%i): %i eq_size, %i units, %i idle \n", sweeper->getLocalId(), eq_size, unit_size, sweeper->sweeper_is_idle);


		std::vector<int> contrib = std::move(eqs);
		contrib.insert(contrib.end(), units.begin(), units.end());

		auto stats = sweeper->fetchSweepStats();
		appendMetadataToReductionElement(contrib, sweeper->sweeper_is_idle, unit_size, eq_size, stats.progress_work_sweeps, stats.progress_work_stepovers, stats.progress_unsched_resweeps);

		contribs.push_back(contrib);
	}

	auto aggregation_element = aggregateEqUnitContributions(contribs);

	LOG(V4_VVER, "SWEEP [%i] contributing ~~~%zu~~~(+%i)~~> to _red \n", _my_rank, aggregation_element.size()-NUM_METADATA_FIELDS, NUM_METADATA_FIELDS);

	if (_terminate_all.load(std::memory_order_relaxed)) {
		LOG(V4_VVER, "SWEEP SHARE BCAST skip contribution, seen already _terminate_all\n");
		return;
	}

	if (okToTrackSharingDelay()) {
		_timestamp_contributed_to_sharing.push_back(Timer::elapsedSeconds());
	}

	_red->contribute(std::move(aggregation_element));

}

void SweepJob::advanceAllReduction() {
	if (!_red)
		return;
	//always keep the global reduction advancing, independently of the state of the local solvers
	_red->advance();
	// LOG(V4_VVER, "SWEEP [%i] SHARE hasResult() %i \n", _my_rank, _red->hasResult());
	if (_red->hasResult()) {
		extractAllReductionResult();
	}
}

void SweepJob::extractAllReductionResult() {
	//There is data from global aggregation, extract it
	assert(_red);
	assert(_red->hasResult());

	auto data = _red->extractResult();
	const int terminate   = data[data.size()-METADATA_TERMINATE];
	const int sweep_iteration = data[data.size()-METADATA_SWEEP_ITERATION];
	const int sharing_round= data[data.size()-METADATA_SHARING_ROUND];
	const int all_idle    = data[data.size()-METADATA_IDLE];
	const int unit_size   = data[data.size()-METADATA_UNIT_SIZE];
	const int eq_size     = data[data.size()-METADATA_EQ_SIZE];
	assert(eq_size%2==0 || log_return_false("SWEEP ERROR: Import Equality size %i not even\n", eq_size));

	if (okToTrackSharingDelay())
		_timestamp_receive_sharing_result.push_back(Timer::elapsedSeconds());


	LOG(V2_INFO, "SWEEP GOTT: iter %i round %i : %i ai , %i trm . E %i  U %i  \n", sweep_iteration, sharing_round, all_idle, terminate, eq_size/2, unit_size);
	// if (_is_root) {
		// LOGGER(_reslogger, V2_INFO, "SWEEP GOTT: iter %i round %i : %i ai , %i trm . E %i  U %i  \n", sweep_iteration, sharing_round, all_idle, terminate, eq_size/2, unit_size);
	// }
	// LOG(V2_INFO, "SWEEP RED SHARE SKIP bc not all init'd yet: iter(%i),round(%i) got: %i EQS, %i UNITS, (%i)all_idle, (%i)terminate. #longidle: %i / %i \n", sweep_iteration, sharing_round, eq_size/2, unit_size, all_idle, terminate, _lastLongtermIdleCount, _nThreads);

#if SWEEP_NEW_IMPORT_VERSION == 0
	//if our local solvers are not fully initialised yet we ignore the global sharing data, is cleaner than going hot solver-by-solver
	if (_started_synchronized_solving) {
		//All solvers are initialised, we can make use of the shared data

		if (eq_size > MAX_IMPORT_SIZE) {
			LOG(V1_WARN, "WARN SWEEP too many equalities to import! %i, max %i\n", eq_size, MAX_IMPORT_SIZE);
		}
		if (unit_size > MAX_IMPORT_SIZE) {
			LOG(V1_WARN, "WARN SWEEP too many units to import! %i, max %i\n", unit_size, MAX_IMPORT_SIZE);
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
					shweep_set_sweep_iteration(sweeper->solver, sweep_iteration);
				}
			}
		}
	} else {
		LOG(V2_INFO, "SWEEP RED SHARE GOTT SKIP: iter(%i),round(%i): not all solvers init'd yet (%i / %i)\n", sweep_iteration, sharing_round, _started_sweepers_count.load(),  _nThreads);
	}

#else

	assert(sharing_round > _lastImportedRound.load() || log_return_false("SWEEP ERROR : unexpected round number when importing shared data. got round %i, while lastImportedRound %i \n", sharing_round, _lastImportedRound.load()));

	assert(_imported_EQS_UNITS[sharing_round].eqs.empty()   || log_return_false("SWEEP ERROR : want to store %i shared eq   integers, but already importedRounds[%i].eqs.size()==%zu nonempty ", eq_size,  sharing_round, _imported_EQS_UNITS[sharing_round].eqs.size()));
	assert(_imported_EQS_UNITS[sharing_round].units.empty() || log_return_false("SWEEP ERROR : want to store %i shared unit integers, but already importedRounds[%i].units.size()==%zu nonempty", unit_size,  sharing_round, _imported_EQS_UNITS[sharing_round].units.size()));

	_imported_EQS_UNITS[sharing_round].eqs   = std::vector<int>(data.begin()		  , data.begin() + eq_size);
	_imported_EQS_UNITS[sharing_round].units = std::vector<int>(data.begin() + eq_size, data.begin() + eq_size + unit_size);
	_lastImportedRound = sharing_round;

	//Sweepers can increase the size of their sweeping environments in later sweep iterations (analog to kissats own increasing environments)
	//We tell them the current iteration, so that they can adjust accordingly
	//We might want to limit the environment increase, since this SweepApp arrives typically at higher iteration numbers than a sequential kissat run, and thus just ever increasing the environments might be too costly or inefficient
	// if (_started_synchronized_solving.load(std::memory_order_relaxed) && !_terminate_all.load(std::memory_order_relaxed) && sweep_iteration <= _params.sweepMaxGrowthIteration.val) {
		// for (auto &sweeper : _sweepers) {
			// if (sweeper) {
				// shweep_set_sweep_iteration(sweeper->solver, sweep_iteration);
			// }
		// }
	// }

#endif

	//the root node is special in that it is the only node that initiates sharing rounds. Prepare for a new one, since we just extracted all the shared data from the current round.
	if (_is_root) {
		LOG(V4_VVER, "SWEEP root: RESET BCAST for next sharing round\n", _my_rank);
		_bcast.reset(new JobTreeBroadcast(getId(), getJobTree().getSnapshot(),
			[this]() {cbContributeToAllReduce();}, TAG_BCAST_INIT));
	}

	//Reduction is finished. we dont need to directly re-create a new reduction object, but can leave it at null (Contrary to the bcast)
	//The new reduction object will be created by the next bcast round when needed
	_red.reset();


	if (_params.sweepIndividualSweepIters.val && all_idle && _started_synchronized_solving  && !_terminate_all) {
		LOG(V4_VVER, "SWEEP sending end_iteration signal\n");
		for (auto &sweeper : _sweepers) {
			if (sweeper) {
				shweep_set_end_iteration_signal(sweeper->solver);
			}
		}
	}
	//Check whether the whole sweep job sould be terminated.
	//We do this check chronologically last in this function, because there might still be useful shared data that we want to import before terminating, and having an earlier termination signal only increases risks for concurrency problems
	if (terminate) {
		_terminate_all = true;
		//update: we now trigger terminations here directly, no longer indirectly via the worksteal callback
		triggerTerminations();
		LOG(V1_WARN, "# \n # \n # --- [%i] got terminate flag, TERMINATING SWEEP JOB ---\n # \n", _my_rank);
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
		_imported_EQS_UNITS[r].eqs.clear();
		_imported_EQS_UNITS[r].units.clear();
		LOG(V4_VVER, "SWEEP [%i] CLEARED round %i data \n", _my_rank, r);
		_lastClearedRound = r;
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
			log_return_false("ERROR in AllReduce, Bad Element Format: Claims total size %i != %zu actual contrib.size() (claims: eq_size %i,  units size %i, metadata %i)", claimed_total_size, contrib.size(), claimed_eq_size, claimed_unit_size, NUM_METADATA_FIELDS)
			);
	}

	//Layout: contrib = [equivalences, units, metadata]

	//Deduplication

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
	int aggr_eq_size   = 2*dedup_eqs.size(); //each key is 64 bit, i.e. 2 32-bit literals
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

	//See whether all solvers are idle
	bool all_idle = true;
    for (const auto &contrib : contribs) {
		bool idle = contrib[contrib.size()-METADATA_IDLE];
    	all_idle &= idle;
    }

	//Aggregate the individual work/resweep counts
	int sum_work_sweeps = 0;
	int sum_work_stepovers = 0;
	int sum_unsched_resweeps = 0;
	for (const auto &contrib : contribs) {
		sum_work_sweeps     += contrib[contrib.size()-METADATA_WORK_SWEEPS];
		sum_work_stepovers  += contrib[contrib.size()-METADATA_WORK_STEPOVERS];
		sum_unsched_resweeps+= contrib[contrib.size()-METADATA_UNSCHED_RESWEEPS];
	}

	if (contribs.empty()) {
		all_idle = false; //edge-case: if not a single solver is initialized yet, we are waiting for them to come online, so they are not really idle
	}

	appendMetadataToReductionElement(aggregated, all_idle, aggr_unit_size, aggr_eq_size, sum_work_sweeps, sum_work_stepovers, sum_unsched_resweeps);

	// if (contribs.size()>1)
	LOG(V4_VVER, "SWEEP RED aggregated %i contributions: E %i, U %i, (%i)allidle\n", contribs.size(), aggr_eq_size/2, aggr_unit_size, all_idle);

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
			LOG(V4_VVER, "SWEEP giv [%i](%i) ===%zu===> [%i](%i) \n",_my_rank, localId, stolen_work.size(), asking_rank, asking_sourceLocalId);
			return stolen_work;
		}
	}
	//no work available at the local rank
	return {};
}

//Syntatic sugar to automatically clear a flag again when it goes out of scope, so we don't have to do it manually
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
	if (_terminate_all.load(std::memory_order_relaxed)) //sweeping finished globally, nothing to steal anymore
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
	//The congruence closure solver will always return 0, as it doesnt operate on work and doesnt have any
	int max_steal_amount = shweep_get_max_steal_amount(sweeper->solver);
	if (max_steal_amount < MIN_STEAL_AMOUNT)
		return {};

	// LOG(V2_INFO, "ß %i max_steal_amount\n", max_steal_amount);
	assert(max_steal_amount > 0			 || log_return_false("SWEEP STEAL ERROR [%i](%i): negative max steal amount %i, maybe segfault into non-initialized kissat solver \n", _my_rank, localId, max_steal_amount));
	assert(max_steal_amount < 2*_numVars || log_return_false("SWEEP STEAL ERROR [%i](%i): too large max steal amount %i >= 2*NUM_VARS, maybe segfault into non-initialized kissat solver \n", _my_rank, localId, max_steal_amount));

	//There is something to steal
	//Prevent that multiple solvers can steal concurrently from the same solver, because that can duplicate work (two threads copying an index it at the very same moment)
	//While duplication of work doesnt affect the overall correctness, such concurrent reads are still a pretty uncontrolled thing, and on top it just creates more work that has to be processed.
	//It also seemed that 23 threads stealing at the same time delayed the first stealing, maybe since the hardware had to constantly synchronize all their cachelines
	scoped_guard steal_guard(sweeper->steal_victim_lock);
	if (!steal_guard.acquired())
		return {};

	//Allocate memory for the steal here in C++, and pass the allocation to kissat for filling
	std::vector<int> stolen_work = std::vector<int>(max_steal_amount);

	// LOG(V3_VERB, "[%i] stealing from (%i), expecting max %i  \n", _my_rank, localId, max_steal_amount);
	int actually_stolen = shweep_steal_from_this_solver(sweeper->solver, reinterpret_cast<unsigned int*>(stolen_work.data()), max_steal_amount);
	assert(actually_stolen >= 0 || log_return_false("SWEEP ERROR : negative stolen amount %i \n", actually_stolen));
	//We allocated the steal array as large as maximally needed, but that was only an upper limit estimate, often during stealing it turns out that we receive a bit less work than estimated
	//So now we know the exact amount of work we stole, and shrink the array to have its .size() match with this stolen amount
	stolen_work.resize(actually_stolen);

	return stolen_work;
	//lock is freed up now automatically by going out of scope
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

	LOG(V3_VERB, "SWEEP [%i](%i) loading formula (%.3f MB) \n", _my_rank, sweeper->getLocalId(), formula_in_MB);
	float t0 = Timer::elapsedSeconds();

	// for (int i = 0; i < payload_size ; i++) {
		// sweeper->addLiteral(lits[i]);
	// }

	constexpr int CHECK_INTERVAL = 50000;
	int counter = CHECK_INTERVAL;
	for (int i = 0; i < payload_size; i++) {
		sweeper->addLiteral(lits[i]);

		if (--counter == 0) {
			counter = CHECK_INTERVAL;
			if (_terminate_all.load(std::memory_order_relaxed)) {
				LOG(V1_WARN, "SWEEP Warn [%i](%i) stopped loading formula due to termination  (at payload lit %i / %i) \n", _my_rank, sweeper->getLocalId(), i, payload_size);
				break;
			}
		}
	}

	float t1 = Timer::elapsedSeconds();
	LOG(V3_VERB, "SWEEP [%i](%i) loaded  formula (%.3f MB) in %.6f sec \n", _my_rank, sweeper->getLocalId(), formula_in_MB , (t1-t0));
}

void SweepJob::triggerTerminations() {
	LOG(V2_INFO, "SWEEP TERM #%i [%i] trigger solver terminations (ctx %i). State before: Running %i, Finished %i \n", getId(), _my_rank, _my_ctx_id, _running_sweepers_count.load(), _finished_sweepers_count.load());
	int i=0;
	for (auto &sweeper : _sweepers) {
		if (sweeper) {
			sweeper->triggerSweepTerminate(_params.sweepIndividualSweepIters.val);
			LOG(V3_VERB, "SWEEP TERM #%i [%i] trigger termination of solver (%i) \n", getId(), _my_rank, i);
		} else {
			LOG(V3_VERB, "SWEEP TERM #%i [%i] skip    termination of solver (%i), already null \n", getId(), _my_rank, i);
		}
		i++;
	}


}

SweepJob::~SweepJob() {
	LOG(V3_VERB, "SWEEP JOB DESTRUCTOR ENTERED (ctx %i) \n", _my_ctx_id);
	for (int i=0; i<5; i++) {
		clearImportedRound();
	}
	if (_terminated_while_synchronizing) {
		LOG(V1_WARN, "SWEEP [%i] Warn : rank was terminated while synchronizing \n", _my_rank);
	}
	if (!_terminated_while_synchronizing && (_lastClearedRound + 2 < _lastImportedRound)) {
		LOG(V1_WARN, "SWEEP [%i] WARN : didn't clear all imported rounds. lastCleared %i, lastImported %i \n", _my_rank, _lastClearedRound, _lastImportedRound.load());
	}
	if (_lastImportedRound==0) {
		LOG(V1_WARN, "SWEEP [%i] WARN : rank didn't receive a single sharing round! \n", _my_rank);
	}
	// triggerTerminations();
	LOG(V3_VERB, "SWEEP JOB DESTRUCTOR DONE\n");
}














