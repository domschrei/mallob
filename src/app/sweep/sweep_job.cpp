
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
    : Job(params, setup, table)
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

// void shweep_set_SweepJob_eq_import_callback(kissat *solver, void *SweepJobState, void (*import_callback) (void *SweepJobState, int localId, int *lit1, int *lit2));


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

	//do not trigger a send on the initial dummy worksteal requests
	_worksteal_requests.resize(_nThreads);
	for (auto &request : _worksteal_requests) {
		request.sent = true;
	}

	//Remember for each solver how many entries of the Eqs/Units he has not yet read, i.e. not yet imported
	// _unread_count_EQS_to_import = std::vector<std::atomic_int>(_nThreads);
	// _unread_count_UNITS_to_import = std::vector<std::atomic_int>(_nThreads);
	_solver_unread_EQS_count.resize(_nThreads);
	_solver_unread_UNITS_count.resize(_nThreads);
	_solver_import_round.resize(_nThreads);

	_worksweeps = std::vector<int>(_nThreads, -1);
	_resweeps_in = std::vector<int>(_nThreads, -1);
	_resweeps_out = std::vector<int>(_nThreads, -1);

	// Now assign 0 to each element
	// for (auto& atomic : _solver_unread_EQS_count) {atomic.store(0);}
	// for (auto& atomic : _solver_unread_UNITS_count) {atomic.store(0);}

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
}



// Called periodically by the main thread to allow the worker to emit messages.
void SweepJob::appl_communicate() {
	LOG(V4_VVER, "SWEEP JOB appl_communicate() \n");

	printIdleFraction();
	sendMPIWorkstealRequests();
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

		LOG(V3_VERB, "SWEEP MSG [%i] received steal request from [%i](%i) \n", _my_rank, sourceRank, localId);
		auto locally_stolen_work = stealWorkFromAnyLocalSolver();

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
		LOG(V3_VERB, "SWEEP MSG to [%i](%i) received steal answer --%i-- from [%i]\n", _my_rank, localId, _worksteal_requests[localId].stolen_work.size(), sourceRank );
	}
	else if (mpiTag == MSG_NOTIFY_JOB_ABORTING)  {
		LOG(V1_WARN, "SWEEP MSG Warn [%i]: received NOTIFY_JOB_ABORTING \n", _my_rank);
	} else if (mpiTag == MSG_NOTIFY_JOB_TERMINATING) {
		LOG(V1_WARN, "SWEEP MSG Warn [%i]: received NOTIFY_JOB_TERMINATING \n", _my_rank);
	} else if (mpiTag == MSG_INTERRUPT) {
		LOG(V1_WARN, "SWEEP MSG Warn [%i]: received MSG_INTERRUPT \n", _my_rank);
	} else {
		LOG(V1_WARN, "SWEEP MSG Warn [%i]: received unexpected mpiTag %i with msg.tag %i \n", _my_rank, mpiTag, msg.tag);
		// assert(log_return_false("ERROR SWEEP MSG, unexpected mpiTag\n"));
	}
}

void SweepJob::appl_terminate() {
	LOG(V2_INFO, "SWEEP JOB id #%i rank [%i] got TERMINATE signal (appl_terminate()) \n", getId(), _my_rank);
	_terminate_all = true;
	_external_termination = true;
	gentlyTerminateSolvers();
}


void SweepJob::createAndStartNewSweeper(int localId) {
	LOG(V3_VERB, "SWEEP JOB [%i](%i) queuing background worker thread\n", _my_rank, localId);
	_bg_workers[localId]->run([this, localId]() {
		LOG(V3_VERB, "SWEEP JOB [%i](%i) WORKER START \n", _my_rank, localId);
		// std::future<void> fut_shweeper = ProcessWideThreadPool::get().addTask([this, localId]() { //Changed from [&] to [this, shweeper] back to [&] to [this, localId] !! (nicco)
		//passing localId by value to this lambda, because the thread might only execute after createAndStartNewShweeper is already gone
		if (_terminate_all) {
			// _bg_workers[localId]->stop();
			LOG(V3_VERB, "SWEEP [%i](%i) terminated before creation\n", _my_rank, localId);
			return;
		}
		_running_sweepers_count++;

		auto sweeper = createNewSweeper(localId);

		loadFormula(sweeper);
		LOG(V3_VERB, "SWEEP JOB [%i](%i) solve() START \n", _my_rank, localId);
		_sweepers[localId] = sweeper; //signal to the remaining system that this solver now exists
		int res = sweeper->solve(0, nullptr);
		LOG(V3_VERB, "SWEEP JOB [%i](%i) solve() FINISH. Result %i \n", _my_rank, localId, res);

		//transfer some solver-specific statistics
		sweeper->fetchSweeperStats();
		auto stats = sweeper->getSolverStatsRef();
		_worksweeps[localId] = stats.sw.worksweeps;
		_resweeps_in[localId] = stats.sw.resweeps_in;
		_resweeps_out[localId] = stats.sw.resweeps_out;

		if (res==20) {
			//Found UNSAT
			assert(kissat_is_inconsistent(sweeper->solver) || log_return_false("SWEEP ERROR: Solver returned UNSAT 20 but is not in inconsistent (==UNSAT) state!\n"));
			int unset_state = -1;
			//Check whether we are the very first solver to report anything. If we are, report and block others
			if (_reporting_localId->compare_exchange_strong(unset_state, localId)) {
				assert(_solved_status==-1);
				serializeResultFormula(sweeper); //there is no result formula, but serialization adds some required metadata to the job result
				reportStats(sweeper, UNSAT);
				_internal_result.result = UNSAT;
				_solved_status = UNSAT;
				//no formula is read, since we are directly UNSAT
			}
		} else if (res==0) {
			//might have made some progress
			if (sweeper->hasReportedSweepDimacs()) {
				//made some progress, and this is the specific solver that reported the final formula
				assert(_solved_status==-1);
				serializeResultFormula(sweeper);
				reportStats(sweeper, IMPROVED);
				_internal_result.result = IMPROVED;
				_solved_status = IMPROVED;
			}
		} else {
			assert(log_return_false("SWEEP ERROR: solver has unexpected return signal %i \n", res));
		}

		_running_sweepers_count--;
		_sweepers[localId]->cleanUp(); //write kissat timing profile
		_sweepers[localId] = nullptr;  //signal that this solver doesnt exist anymore
		LOG(V3_VERB, "SWEEP JOB [%i](%i) WORKER EXIT\n", _my_rank, localId);

		if (_running_sweepers_count==0) {
			//we are the very last solver, so by now all stats from all solvers should be collected
			//doing this here ensures that this will be printed regardless of how exactly the SweepJob terminated (by itself or externally)
			printResweeps();
		}
	});
	// _fut_shweepers.push_back(std::move(fut_shweeper));
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
	LOG(V3_VERB, "SWEEP STARTUP [%i](%i) kissat init %f ms\n", _my_rank, localId, init_dur_ms);
	if (init_dur_ms > WARN_init_dur) {
		LOG(V1_WARN, "SWEEP Warn STARTUP [%i](%i): kissat init took unusually long, %f ms !\n", _my_rank, localId, init_dur_ms);
	}

	//Dangerous to immediately return here! because kissat is already initialized, can't just forget it, need to properly release it
	//releasing could potentially be done directly here with kissat_release(...) ....
	//for now leaving it to finish initialising and receiving termination through the normal procedure
	// if (_terminate_all) {
		// LOG(V2_INFO, "SWEEP [%i](%i) terminated during creation \n", _my_rank, localId);
		// return shweeper;
	// }

	sweeper->setToSweeper();

	//Connecting kissat to Kissat
	sweeper->sweepSetImportExportCallbacks();

	//Connecting kissat directly to SweepJob
    shweep_set_search_work_callback(sweeper->solver, this, cb_search_work_in_tree);
	shweep_set_SweepJob_eq_import_callback(sweeper->solver, this, cb_import_eq);
	shweep_set_SweepJob_unit_import_callback(sweeper->solver, this, cb_import_unit);

	if (_is_root) {
		//read out final formula only at the root node
		sweeper->sweepSetReportCallback();
		sweeper->sweepSetReportingPtr(_reporting_localId);
	}

    //Basic configuration
    sweeper->set_option("quiet", 0);  //suppress any standard kissat messages
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
	//Specific for clean sweep run
	sweeper->set_option("preprocess", 0); //skip other preprocessing stuff after shweep finished
	// shweeper->set_option("probe", 1);   //there is some cleanup-probing at the end of the sweeping. keep it? (apparently the probe option is used nowhere anyways)
	sweeper->set_option("substitute", 1); //apply equivalence substitutions after sweeping (kissat default 1, but keep here explicitly to remember it)
	sweeper->set_option("substituterounds", 2); //default is 2, and changing that has currently no effect, virtually all substitutions happen in round 1, and already in round 2 zero or single substitutions are found, and it exits there.
	// shweeper->set_option("substituteeffort", 1000); //modification doesnt seem to have much effect...
	// shweeper->set_option("substituterounds", 10);
	sweeper->set_option("luckyearly", 0); //skip
	sweeper->set_option("luckylate", 0);  //skip
	sweeper->interruptionInitialized = true;
	return sweeper;
}

void SweepJob::serializeResultFormula(KissatPtr sweeper) {
	LOG(V2_INFO, "SWEEP JOB [%i](%i) serializing result formula from solver \n", _my_rank, sweeper->getLocalId());
	// assert(shweeper->hasPreprocessedFormula());
	std::vector<int> formula = sweeper->extractPreprocessedFormula(); //format is [Clauses, #Vars, #Clauses], i.e. clauses followed by two metadata-ints at the end. Or empty, if the solver found UNSAT or made no progress at all
	_internal_result.setSolutionToSerialize(formula.data(), formula.size()); //we always need to serialize, even an empty formula, because serialization adds some required metadata in front
}

void SweepJob::reportStats(KissatPtr sweeper, int res) {
	sweeper->fetchSweeperStats();
	auto stats = sweeper->getSolverStats();
	//As "vars" we are only interested in variables that we still active (not fixed) at the start of Sweep. The "VAR" counter is much larger, but most of these variables are often already fixed.
	int units_orig = stats.sw.total_units - stats.sw.new_units;
	int vars_orig = stats.sw.active_orig + units_orig;
	int vars_fixed = stats.sw.eqs + stats.sw.sweep_units;
	//actual_done is a slightly conservative count, because we only include the units found by the sweeping algorithm itself,
	//and dont include some stray units found while propagating the sweep decisions (that would be stats.shweep_new_units)
	int vars_remain = vars_orig - vars_fixed;
	int clauses_removed = stats.sw.clauses_orig - stats.sw.clauses_end;

	double vars_fixed_percent = 100*vars_fixed/(double)vars_orig;
	double vars_remain_percent = 100*vars_remain/(double)vars_orig;
	double clauses_removed_percent = 100*clauses_removed/(double)stats.sw.clauses_orig;

	LOG(V2_INFO, "RESULT SWEEP_CODE			  %i res code\n", res);
	LOG(V2_INFO, "RESULT SWEEP_TIME           %f seconds \n", Timer::elapsedSeconds() - _start_sweep_timestamp);
	LOG(V2_INFO, "RESULT SWEEP_PRIORITY       %f\n", _params.preprocessSweepPriority.val);
	LOG(V2_INFO, "RESULT SWEEP_PROCESSES      %i\n", getVolume());
	LOG(V2_INFO, "RESULT SWEEP_THREADS_PER_P  %i\n", _nThreads);
	LOG(V2_INFO, "RESULT SWEEP_SHARING_PERIOD %i ms \n", _params.sweepSharingPeriod_ms.val);
	LOG(V2_INFO, "RESULT SWEEP_VARS_ORIG      %i\n", stats.sw.vars_orig);
	LOG(V2_INFO, "RESULT SWEEP_VARS_END       %i\n", stats.sw.vars_end);
	LOG(V2_INFO, "RESULT SWEEP_ACTIVE_ORIG    %i\n", stats.sw.active_orig);
	LOG(V2_INFO, "RESULT SWEEP_ACTIVE_END     %i\n", stats.sw.active_end);
	LOG(V2_INFO, "RESULT SWEEP_CLAUSES_ORIG   %i\n", stats.sw.clauses_orig);
	LOG(V2_INFO, "RESULT SWEEP_CLAUSES_END    %i\n", stats.sw.clauses_end);
	LOG(V2_INFO, "RESULT SWEEP_UNITS_ORIG     %i\n", units_orig);
	LOG(V2_INFO, "RESULT SWEEP_UNITS_NEW      %i\n", stats.sw.new_units);
	LOG(V2_INFO, "RESULT SWEEP_UNITS_TOTAL    %i\n", stats.sw.total_units);
	LOG(V2_INFO, "RESULT SWEEP_ELIMINATED     %i\n", stats.sw.eliminated);
	LOG(V2_INFO, "RESULT SWEEP_EQUIVALENCES   %i\n", stats.sw.eqs);
	LOG(V2_INFO, "RESULT SWEEP_UNITS_SWEEP    %i\n", stats.sw.sweep_units);
	LOG(V2_INFO, "RESULT SWEEP_VARS_REMAIN_N    %i / %i (%.2f %)\n", vars_remain, vars_orig, vars_remain_percent);
	LOG(V2_INFO, "RESULT SWEEP_VARS_FIXED_N     %i / %i (%.2f %)\n", vars_fixed, vars_orig, vars_fixed_percent);
	LOG(V2_INFO, "RESULT SWEEP_VARS_FIXED_PRCNT %.2f \n", vars_fixed_percent);
	LOG(V2_INFO, "RESULT SWEEP_CLAUSES_REMOVED_N %i \n", clauses_removed);
	LOG(V2_INFO, "RESULT SWEEP_CLAUSES_REMOVED_PRCNT %.2f \n", clauses_removed_percent);

	for (int i=0; i < _sharing_start_ping_timestamps.size() && i < _sharing_receive_result_timestamps.size(); i++) {
		float start = _sharing_start_ping_timestamps[i];
		float end   = _sharing_receive_result_timestamps[i];
		LOG(V2_INFO, "RESULT SWEEP_SHARING_LATENCY  %f ms   (ping->result  %f --> %f) \n", (end-start)*1000, start, end);
	}
	for (int i=0; ! _sharing_start_ping_timestamps.empty() && i < _sharing_start_ping_timestamps.size() -1; i++) {
		LOG(V2_INFO, "RESULT SWEEP_SHARING_PERIOD_REAL  %f ms \n", (_sharing_start_ping_timestamps[i+1] - _sharing_start_ping_timestamps[i])*1000);
	}

	LOG(V3_VERB, "RESULT SWEEP [%i](%i) Serialized final formula to SolutionSize=%i\n", _my_rank, _reporting_localId->load(), _internal_result.getSolutionSize());
	for (int i=0; i<15 && i<_internal_result.getSolutionSize(); i++) {
		LOG(V3_VERB, "RESULT Sweep Formula peek %i: %i \n", i, _internal_result.getSolution(i));
	}


	LOG(V2_INFO, "SWEEP JOB [%i](%i) completed readStats \n", _my_rank, sweeper->getLocalId());
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
	LOG(V2_INFO, "RESULT %i SWEEP WORKSWEEPS,RESWEEPS: %s \n", _my_rank, oss.str().c_str());
	LOG(V2_INFO, "RESULT %i SWEEP_WORKSWEEPS %i \n", _my_rank, worksweeps);
	LOG(V2_INFO, "RESULT %i SWEEP_RESWEEPS_IN %i \n", _my_rank, resweeps_in);
	LOG(V2_INFO, "RESULT %i SWEEP_RESWEEPS_OUT %i \n", _my_rank, resweeps_out);
}

void SweepJob::sendMPIWorkstealRequests() {
	//Worksteal requests need to be execute by the MPI main thread, as it can be problematic if the kissat-threads issue MPI messages in the callback on their own
	//Thus the solver-threads only write a request in the callback, and that is picked up here by the main MPI thread

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
			LOG(V3_VERB, "SWEEP MSG sending MPI request [%i](%i) -> [%i] \n", _my_rank, request.localId, request.targetRank);
			getJobTree().send(request.targetRank, MSG_SEND_APPLICATION_MESSAGE, msg);
		}
	}
}

void SweepJob::cbImportEq(int *ilit1, int *ilit2, int localId) {
	if (_solver_unread_EQS_count[localId] == 0) {
		// *ilit1 = 0;
		// *ilit2 = 0;
		return;
	}

	{
		std::shared_lock<std::shared_mutex> lock(_EQS_import_mutex);
		int size = 	_EQS_to_import.size();
		assert(_rank_import_round >= _solver_import_round[localId] || log_return_false("SWEEP ERROR: in cbImportEq: solver_import_round (%i) larger than rank_import_round (%i) \n", _rank_import_round, _solver_import_round[localId]));
		if (_rank_import_round > _solver_import_round[localId]) {
			_solver_import_round[localId] = _rank_import_round;
			_solver_unread_EQS_count[localId] = size;
			if (size==0)
				return;
		}
		int unread = _solver_unread_EQS_count[localId];

		assert(unread <= size || log_return_false("SWEEP ERROR: in cbImportEq: unread (%i) larger than EQS_to_import size (%i) \n", unread, size));
		//We still want to read the array from front to back for prefetching efficiency, so we invert the reading index
		//We stored unread instead of read, because via unread==0 we can quickly tell whether we are done, whereas with read we would need to compare with the array size each time
		int idx = size - unread;
		assert((idx >= 0 && idx+1 < size) || log_return_false("SWEEP ERROR: in cbImportEq: read idx (%i) out of bounds (size %i)\n", idx, size));
		*ilit1 = _EQS_to_import[idx];
		*ilit2 = _EQS_to_import[idx+1];
		_solver_unread_EQS_count[localId] -= 2; //have processed these two literals
		assert(*ilit1 < *ilit2 || log_return_false("SWEEP ERROR: in cbImportEq: *ilit1 %i !< %i *ilit2, should be sorted\n", *ilit1, *ilit2));
		assert(_solver_unread_EQS_count[localId] >= 0 || log_return_false("SWEEP ERROR: in cbImportEq: negative unread count %i \n", _solver_unread_EQS_count[localId]));
	}
	//now returning to kissat solver
}

void SweepJob::cbImportUnit(int *ilit, int localId) {
	// int unread_count = _solver_unread_UNITS_count[localId];
	if ( _solver_unread_UNITS_count[localId]== 0) {
		// *ilit = INVALID_LIT;
		return;
	}

	{
		std::shared_lock<std::shared_mutex> lock(_UNITS_import_mutex);
		int size = _UNITS_to_import.size();
		assert(_rank_import_round >= _solver_import_round[localId] || log_return_false("SWEEP ERROR: in cbImportUnit: solver_import_round (%i) larger than rank_import_round (%i) \n", _rank_import_round, _solver_import_round[localId]));
		if (_rank_import_round > _solver_import_round[localId]) {
			_solver_import_round[localId] = _rank_import_round;
			_solver_unread_UNITS_count[localId] = size;
			if (size==0)
				return;
		}
		int unread = _solver_unread_UNITS_count[localId];
		assert(unread <= size || log_return_false("SWEEP ERROR: in cbImportUnit: unread (%i) larger than UNITS_to_import size (%i) \n", unread, size));
		int idx = size - unread;
		assert((idx >= 0 && idx+1 < size) || log_return_false("SWEEP ERROR: in cbImportUnit: read idx (%i) out of bounds (size %i)\n", idx, size));
		*ilit = _UNITS_to_import[idx];
		_solver_unread_UNITS_count[localId]--;
		assert(_solver_unread_UNITS_count[localId] >= 0 || log_return_false("SWEEP ERROR: in cbImportUnit: negative unread count , %i \n", _solver_unread_UNITS_count[localId]));
	}
	//now returning to kissat solver
}


void SweepJob::cbSearchWorkInTree(unsigned **work, int *work_size, int localId) {
	KissatPtr sweeper = _sweepers[localId]; //array access safe (we know the sweeper exists) because this callback is called by this sweeper itself
	//shweeper->shweeper_is_idle = true; //made idle flag more fine grained, because it caused a race condition here for the very first solver that was marked idle while it was receiving the initial work
	sweeper->work_received_from_steal = {};

	//loop until we find work or the whole sweeping is terminated
	while (true) {
		LOG(V5_DEBG, "Sweeper [%i](%i) in steal loop\n", _my_rank, localId);

		if (_terminate_all) {
			//this is the signal for the solver to terminate itself, by sending it a work array of size 0
			sweeper->work_received_from_steal = {};
			sweeper->sweeper_is_idle = true;
			LOG(V3_VERB, "Sweeper [%i](%i) steal loop - node got terminate signal, exit loop\n", _my_rank, localId);
			break;
		}
		 /*
		  * At the root node we serve the initial work to whichever solver asks first
		  */
		if (_is_root && ! _root_provided_initial_work) {
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
			LOG(V2_INFO, "SWEEP WORK PROVIDED: All work --------------%u---------------->sweeper [%i](%i)\n", VARS, _my_rank, localId);
			break;

		}

		//Try to steal locally from shared memory
		 /*
		  * Going for some direct global steals doesnt do much here, because the big rally happens anyways the moment all are depleted locally
		  * At that point they anyways schedule a global sweep, and some sweepers stealing globally before cant stop that
		  */

		LOG(V5_DEBG, "SWEEP WORK [%i](%i) steal loop --> local steal \n", _my_rank, localId);
		sweeper->sweeper_is_idle = true;
		auto stolen_work = stealWorkFromAnyLocalSolver();

		//Successful local steal
		if ( ! stolen_work.empty()) {
			//store steal data persistently in C++, such that C can keep operating on that memory segment
			sweeper->work_received_from_steal = std::move(stolen_work);
			LOG(V3_VERB, "SWEEP WORK [%i] ---%i---> (%i) \n", _my_rank, sweeper->work_received_from_steal.size(), localId);
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
				LOG(V3_VERB, "SWEEP WORK [%i](%i) steal loop: aborts waiting for MPI response - node got terminate signal \n", _my_rank, localId);
				break;
			}
		}
		//Successful steal if size > 0
		if ( ! _worksteal_requests[localId].stolen_work.empty()) {
			sweeper->work_received_from_steal = std::move(_worksteal_requests[localId].stolen_work);
			LOG(V3_VERB, "SWEEP WORK via MPI [%i] ---%i---> [%i](%i) \n", targetRank, sweeper->work_received_from_steal.size(), _my_rank, localId);
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

	if (Timer::elapsedSeconds() < _last_sharing_start_timestamp + _params.sweepSharingPeriod_ms.val/1000.0) //convert to seconds
		return;


	//make sure that only one sharing operation is going on at a time
	if (_bcast->hasReceivedBroadcast()) {
		LOG(V3_VERB, "SWEEP SHARE BCAST: Would like to initiate new sharing round, but old round is not completed yet\n");
		return;
	}
	//Broadcast a ping to all workers to initiate an AllReduce
	//The broadcast includes all workers currently reachable by the root-node and informs them about their parent and potential children
	//It then causes the leaf nodes to call the callback, initiating the AllReduce
	_last_sharing_start_timestamp = Timer::elapsedSeconds();
	_sharing_start_ping_timestamps.push_back(_last_sharing_start_timestamp);
	LOG(V3_VERB, "SWEEP SHARE BCAST Initiating Sharing via Ping\n");
	JobMessage msg = getMessageTemplate();
	msg.tag = _bcast->getMessageTag();
	msg.payload = {};
	_bcast->broadcast(std::move(msg));
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


	//Bring individual data per thread in the sharing element format: [Equivalences, Units, eq_size, unit_size, all_idle]
	std::list<std::vector<int>> contribs;
	int id=-1; //for debugging
	for (auto &sweeper : _sweepers) {
		id++;
		if (!sweeper) {
			LOG(V4_VVER, "SWEEP SHARE [%i](%i) not yet initialized, skipped in contribution aggregation \n", _my_rank, id);
			continue;
		}

		//Mutex, because the solvers are asynchronously pushing new eqs/units into these vectors all the time (including reallocations after push_back), make sure that doesn't happen while we copy/move
		std::vector<int> eqs, units;
		{
			std::lock_guard<std::mutex> lock(sweeper->sweep_sharing_mutex);
			eqs = std::move(sweeper->eqs_to_share);
			units = std::move(sweeper->units_to_share);
		}

		int eq_size = eqs.size();
		int unit_size = units.size();
		assert(eq_size%2==0 || log_return_false("ERROR in AGGR: Non-even number %i of equivalence literals, should always come in pairs", eq_size)); //equivalences come always in pairs
		//we need to glue together equivalences and units. can use move on the equivalences to save a copying of them, and only need to copy the units
		//moved logging before the actions, because this code triggered std::bad_alloc once, might give some more info next time
		LOG(V4_VVER, "SWEEP SHARE REDUCE (%i): %i eq_size, %i units, %i idle \n", sweeper->getLocalId(), eq_size, unit_size, sweeper->sweeper_is_idle);
		std::vector<int> contrib = std::move(eqs);
		contrib.insert(contrib.end(), units.begin(), units.end());
		contrib.push_back(eq_size);
		contrib.push_back(unit_size);
		contrib.push_back(sweeper->sweeper_is_idle);

		contribs.push_back(contrib);

		// shweeper->units_to_share.clear();
		// shweeper->eqs_to_share.clear(); //implicitly already happened due to move, but keep here for clarity
	}

	// LOG(V3_VERB, "SWEEP Aggregate contributions within process\n");
	auto aggregation_element = aggregateEqUnitContributions(contribs);

	LOG(V3_VERB, "SWEEP SHARE REDUCE [%i]: contribute size %i (+%i) to sharing\n", _my_rank, aggregation_element.size()-NUM_METADATA_FIELDS, NUM_METADATA_FIELDS);

	if (_terminate_all) {
		LOG(V4_VVER, "SWEEP SHARE BCAST skip contribution, seen already _terminate_all\n");
		return;
	}

	_red->contribute(std::move(aggregation_element));

}

void SweepJob::advanceAllReduction() {
	if (!_red) return;
	LOG(V3_VERB, "SWEEP SHARE REDUCE ADVANCE [%i]\n", _my_rank);
	_red->advance();
	if (!_red->hasResult()) return;

	// LOG(V3_VERB, "[sweep] all-reduction complete\n");
	_sharing_receive_result_timestamps.push_back(Timer::elapsedSeconds());


	//signal to the solvers that they should temporarily not read from the vector because we are reallocating it
	// for (int i=0; i < _nThreads; i++) {
		// if (_unread_count_EQS_to_import[i] != 0)
			// LOG(V3_VERB, "SWEEP Warn: Solver %i is still reading Eqs (%i left) while we want to update them! \n", i, _unread_count_EQS_to_import[i].load());
		// if (_unread_count_UNITS_to_import[i] != 0)
			// LOG(V3_VERB, "SWEEP Warn: Solver %i is still reading units (%i left) while we want to update them! \n", i, _unread_count_UNITS_to_import[i].load());
		// _unread_count_EQS_to_import[i] = 0;
		// _unread_count_UNITS_to_import[i] = 0;
	// }
	// usleep(10); //give solver some short time to recognize that we set unread to zero

	//todo: Filter out duplicates during aggregating?

	//Extract shared data
	auto data = _red->extractResult();
	const int eq_size = data[data.size()-METADATA_EQ_SIZE_POS];
	const int unit_size = data[data.size()-METADATA_UNIT_SIZE_POS];
	const int all_idle = data[data.size()-METADATA_IDLE_FLAG_POS];
	if (_is_root)
		LOG(V2_INFO, "SWEEP SHARE REDUCE RECEIVED: %i EQS, %i UNITS\n", eq_size/2, unit_size);
	else
		LOG(V3_VERB, "SWEEP SHARE REDUCE RECEIVED: %i EQS, %i UNITS\n", eq_size/2, unit_size);

	 /*
	  * We want to prevent concurrent read/write on EQS_to_import vector
	  * Right now the solution is that we have the atomic "unread" flag, as a kind of signal
	  * Together with the 10us sleep above, this should be more than enough time for the solvers to see it
	  * The last potential problem occurs if onread=0 is set AFTER the solvers have acced a valid array element but BEFORE they decrement their unread points
	  * but that would require a race-condition within nanoseconds, which shouldnt occur in our benign usecase
	  */
	{
		std::unique_lock<std::shared_mutex> lock(_EQS_import_mutex);
		std::unique_lock<std::shared_mutex> lock2(_UNITS_import_mutex);
		_EQS_to_import.assign(data.begin(),             data.begin() + eq_size);
		_UNITS_to_import.assign(data.begin() + eq_size, data.end() - NUM_METADATA_FIELDS);
		_rank_import_round++;
	}

		// for (int i=0; i < _nThreads; i++) {
			// _unread_count_EQS_to_import[i] = eq_size;
		// }
		// for (int i=0; i < _nThreads; i++) {
			// _unread_count_EQS_to_import[i] = eq_size;
		// }
	//signal the solvers that there is new data to read

	//Now we can reset the root node, because the sharing operation (broadcast + allreduce) is finished and can prepare a new one
	if (_is_root) {
		LOG(V4_VVER, "SWEEP SHARE [%i] RESET root BCAST\n", _my_rank);
		_bcast.reset(new JobTreeBroadcast(getId(), getJobTree().getSnapshot(),
			[this]() {cbContributeToAllReduce();}, TAG_BCAST_INIT));
	}

	LOG(V4_VVER, "SWEEP SHARE [%i] RESET REDUCE\n", _my_rank);
	_red.reset();

	//Publish termination signal only AFTER we copied the Eqs&Units, as the solvers will immediately try to import these last E&U after exiting their main loop
	if (all_idle) {
		_terminate_all = true;
		LOG(V1_WARN, "# \n # \n # --- ALL SWEEPERS IDLE - CAN TERMINATE -- \n # \n");
	}
}



std::vector<int> SweepJob::aggregateEqUnitContributions(std::list<std::vector<int>> &contribs) {
	//Each contribution has the format [Equivalences,Units, eq_size,unit_size,all_idle].
	//									-----data---------  ------metadata------------

	std::ostringstream oss_tot;
	oss_tot << "Contrib sizes: ";
	// for (int id : rand_permutation) {
		// oss << id << ' ';
	// }
	// LOG(V4_VVER, "%s \n", oss.str().c_str());

	//sanity check whether each contribution contains coherent size information about itself
	for (const auto& contrib : contribs) {
		int claimed_eq_size = contrib[contrib.size()- METADATA_EQ_SIZE_POS];
		int claimed_unit_size = contrib[contrib.size()- METADATA_UNIT_SIZE_POS];
		int claimed_total_size = claimed_eq_size + claimed_unit_size + NUM_METADATA_FIELDS;
		assert(contrib.size() == claimed_total_size ||
			log_return_false("ERROR in AllReduce, Bad Element Format: Claims total size %i != %i actual contrib.size() (claims: eq_size %i,  units size %i, metadata %i)", claimed_total_size, contrib.size(), claimed_eq_size, claimed_unit_size, NUM_METADATA_FIELDS)
			);
	}


	LOG(V3_VERB, "SWEEP AGGR %i contributions \n", contribs.size());
	size_t total_aggregated_size = NUM_METADATA_FIELDS; //the new element will contain the metadata once at the end
	int i=0;
    for (const auto& contrib : contribs) {
	    total_aggregated_size += contrib.size()-NUM_METADATA_FIELDS; //we will copy everything but the metadata from each contribution
		LOG(V4_VVER, "SWEEP AGGR Element %i: contrib.size() %i, w/o metadata %i, curr summed size %i \n", i, contrib.size(), contrib.size()-NUM_METADATA_FIELDS, total_aggregated_size);
		oss_tot << "| " << i << ":" << contrib.size() << " "	;
		i++;
    	assert(contrib.size() >= NUM_METADATA_FIELDS || log_return_false("ERROR in Aggregating: contrib with too small size() == %i < %i SHARING_METADATA_FIELDS", contrib.size(), NUM_METADATA_FIELDS));
    }



	LOG(V4_VVER, "%s \n", oss_tot.str().c_str());
	std::ostringstream oss_e;
	oss_e << "Eq sizes: ";
	i=0;


    std::vector<int> aggregated;
    aggregated.reserve(total_aggregated_size);
	//Fill equivalences
	size_t aggr_eq_size = 0;
    for (const auto &contrib : contribs) {
    	int eq_size = contrib[contrib.size()-METADATA_EQ_SIZE_POS];
    	aggr_eq_size += eq_size;
        aggregated.insert(aggregated.end(), contrib.begin(), contrib.begin()+eq_size);
		oss_e << "| " << i << ":" << eq_size << " "	;
    	i++;
    }

	LOG(V4_VVER, "%s \n", oss_e.str().c_str());
	std::ostringstream oss_u;
	oss_u << "Unit sizes: ";
	i=0;

	//Fill units
	size_t aggr_unit_size = 0;
    for (const auto &contrib : contribs) {
    	int eq_size = contrib[contrib.size()-METADATA_EQ_SIZE_POS];  //need to know where the eq ends, i.e. where the units start
    	int unit_size = contrib[contrib.size()-METADATA_UNIT_SIZE_POS];
		aggr_unit_size += unit_size;
		// LOG(V3_VERB, "SWEEP AGGR Element: %i unit_size \n", unit_size);
        aggregated.insert(aggregated.end(), contrib.begin()+eq_size, contrib.end()-NUM_METADATA_FIELDS); //not copying the metadata at the end
    	oss_u << "| " << i << ":" << unit_size << " "	;
    	i++;
    }

	LOG(V4_VVER, "%s \n", oss_u.str().c_str());

	//See whether all solvers are idle
	bool all_idle = true;
    for (const auto &contrib : contribs) {
		bool idle = contrib[contrib.size()-METADATA_IDLE_FLAG_POS];
    	all_idle &= idle;
		// LOG(V3_VERB, "SWEEP AGGR Element: idle == %i \n", idle);
    }

	if (contribs.empty()) {
		all_idle = false; //edge-case: not a single solver is initialized yet, we are waiting for them to come online, they are not idle
	}

	//these are the three fields we account for with SHARING_METADATA_FIELDS
	aggregated.push_back(aggr_eq_size);
	aggregated.push_back(aggr_unit_size);
	aggregated.push_back(all_idle);
	LOG(V3_VERB, "SWEEP SHARE REDUCE aggregated %i equivalences, %i units, %i all_idle\n", aggr_eq_size/2, aggr_unit_size, all_idle);
	int individual_sum =  aggr_eq_size + aggr_unit_size + NUM_METADATA_FIELDS;
	assert(total_aggregated_size == individual_sum ||
		log_return_false("SWEEP ERROR: aggregated element assert failed: total_size %i != %i individual_sum (total_eq_size %i + total_unit_size %i + metadata %i) ",
			total_aggregated_size, individual_sum, aggr_eq_size, aggr_unit_size, NUM_METADATA_FIELDS));
    return aggregated;
}



std::vector<int> SweepJob::stealWorkFromAnyLocalSolver() {
	auto rand_permutation = getRandomIdPermutation();

	// std::ostringstream oss;
	// oss << "Steal permutation: ";
	// for (int id : rand_permutation) {
		// oss << id << ' ';
	// }
	// LOG(V4_VVER, "%s \n", oss.str().c_str());

	for (int localId : rand_permutation) {
		auto stolen_work = stealWorkFromSpecificLocalSolver(localId);
		if ( ! stolen_work.empty()) {
			LOG(V3_VERB, "SWEEP WORK (%i) ---%i---> \n", localId, stolen_work.size());
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
	LOG(V3_VERB, "SWEEP JOB TERM #%i [%i] interrupting solvers\n", getId(), _my_rank);
	//each sweeper checks constantly for the interruption signal (on the ms scale or faster), allow for gentle own exit
	while (_running_sweepers_count>0) {
		LOG(V3_VERB, "SWEEP JOB TERM #%i [%i] still %i solvers running\n", getId(), _my_rank, _running_sweepers_count.load());
		int i=0;
		for (auto &sweeper : _sweepers) {
			if (sweeper) {
				sweeper->setSweepTerminate();
				LOG(V3_VERB, "SWEEP JOB TERM #%i [%i] terminating solver (%i)\n", getId(), _my_rank, i);
			}
			i++;
		}
		usleep(1000);
	}
	LOG(V3_VERB, "SWEEP JOB TERM #%i [%i] no more solvers running\n", getId(), _my_rank);

	usleep(2000);

	int i=0;
	LOG(V3_VERB, "SWEEP JOB TERM #%i [%i] joining bg_workers \n",  getId(),_my_rank);
	for (auto &bg_worker : _bg_workers) {
		if (bg_worker->isRunning()) {
			LOG(V3_VERB, "SWEEP JOB TERM #%i [%i] joining bg_worker (%i) \n",  getId(),_my_rank, i);
			bg_worker->stop();
			LOG(V3_VERB, "SWEEP JOB TERM #%i [%i] joined  bg_worker (%i) \n",  getId(),_my_rank, i);
		}
		i++;
	}
	LOG(V3_VERB, "SWEEP JOB TERM #%i [%i] joined all bg_workers \n", getId(),_my_rank);
	LOG(V3_VERB, "SWEEP JOB TERM #%i [%i] DONE \n", getId(),_my_rank);
}

SweepJob::~SweepJob() {
	LOG(V3_VERB, "SWEEP JOB DESTRUCTOR ENTERED \n");
	gentlyTerminateSolvers();
	LOG(V3_VERB, "SWEEP JOB DESTRUCTOR DONE\n");
}














