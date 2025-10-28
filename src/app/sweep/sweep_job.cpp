
#include "sweep_job.hpp"

#include "app/job.hpp"
#include "app/job_tree.hpp"
#include "util/ctre.hpp"
#include "util/logger.hpp"
#include "util/sys/tmpdir.hpp"

extern "C" {
#include "kissat/src/kissat.h"
}

#define INIT_PARALLEL 1

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


void SweepJob::appl_start() {
	_my_rank = getJobTree().getRank();
	_my_index = getJobTree().getIndex();
	_is_root = getJobTree().isRoot();
	LOG(V2_INFO,"SWEEP JOB SweepJob appl_start() STARTED: Rank %i, Index %i, ContextId %i, is root? %i, Parent-Rank %i, Parent-Index %i, numThreadsPerProcess=%d\n",
		_my_rank, _my_index, getJobTree().getContextId(), _is_root, getJobTree().getParentNodeRank(), getJobTree().getParentIndex(), _params.numThreadsPerProcess.val);
	LOG(V2_INFO,"SWEEP JOB sweep-sharing-period: %i ms\n", _params.sweepSharingPeriod_ms.val);
    _metadata = getSerializedDescription(0)->data();
	_start_shweep_timestamp = Timer::elapsedSeconds();
	_last_sharing_start_timestamp = Timer::elapsedSeconds();

	//do not trigger a send on the initial dummy worksteal requests
	_worksteal_requests.resize(_params.numThreadsPerProcess.val);
	for (auto &request : _worksteal_requests) {
		request.sent = true;
	}

	//the local IDs will be shuffled for each workstealing request
	for (int localId=0; localId < _params.numThreadsPerProcess.val; ++localId) {
		_list_of_ids.push_back(localId);
	}
	_shweepers.resize(_params.numThreadsPerProcess.val);

	_bg_workers.reserve(_params.numThreadsPerProcess.val);
	for (int i = 0; i < _params.numThreadsPerProcess.val; ++i) {
		_bg_workers.emplace_back(std::make_unique<BackgroundWorker>());
	}

	//a broadcast object is used to initiate an all-reduction by first pinging each processes currently reachable by the root node
	//the ping detects the current tree structure and provides a callback to contribute to the all-reduction
	// LOG(V2_INFO, "[SWEEP] initialize broadcast object\n");

	LOG(V4_VVER, "SWEEP SHARE [%i] RESET BCAST\n", _my_rank);
	_bcast.reset(new JobTreeBroadcast(getId(), getJobTree().getSnapshot(), [this]() {cbContributeToAllReduce();}, TAG_BCAST_INIT));

	//Start individual Kissat threads (those then immediately jump into the sweep algorithm)
	//To keep appl_start() responsive, everything is outsourced to the individual threads
	//(Improvement form earlier initialization which was still done by the main thread, takes ca. 4ms per solver, with x32 threads this resulted in being stuck here for 150ms!
	for (int localId=0; localId < _params.numThreadsPerProcess.val; localId++) {
		#if INIT_PARALLEL==0
			auto shweeper = createNewShweeper(localId);
			_shweepers[localId] = shweeper;
		#endif
		createAndStartNewShweeper(localId);
	}

	LOG(V2_INFO, "SWEEP JOB appl_start() FINISHED\n");
}

void SweepJob::createAndStartNewShweeper(int localId) {
	LOG(V2_INFO, "SWEEP JOB [%i](%i) queuing background worker thread\n", _my_rank, localId);
	_bg_workers[localId]->run([this, localId]() {
	// std::future<void> fut_shweeper = ProcessWideThreadPool::get().addTask([this, localId]() { //Changed from [&] to [this, shweeper] back to [&] to [this, localId] !! (nicco)
		//passing localId by value to this lambda, because the thread might only execute after createAndStartNewShweeper is already gone
		#if INIT_PARALLEL
				auto shweeper = createNewShweeper(localId);
		#else
		auto &shweeper = _shweepers[localId];
		#endif
		if (_terminate_all) {
			_bg_workers[localId]->stop();
			LOG(V2_INFO, "SWEEP JOB TERMINATE [%i](%i) terminated new shweeper directly before it even started, job already done \n", _my_rank, localId);
			return;
		}

		loadFormula(shweeper);
		LOG(V2_INFO, "SWEEP JOB [%i](%i) START solve() \n", _my_rank, localId);
		_running_shweepers_count++;
		#if INIT_PARALLEL
		_shweepers[localId] = shweeper;
		#endif
		int res = shweeper->solve(0, nullptr);
		LOG(V2_INFO, "SWEEP JOB [%i](%i) FINISH solve(). Result %i \n", _my_rank, localId, res);

		assert( ! _is_root || _dimacsReport_localId->load() != -1);
		if (_is_root && localId == _dimacsReport_localId->load()) {
			readResult(shweeper);
		}
		_running_shweepers_count--;
		_shweepers[localId]->cleanUp(); //write profile
		// );
	});
	// _fut_shweepers.push_back(std::move(fut_shweeper));
}


std::shared_ptr<Kissat> SweepJob::createNewShweeper(int localId) {
	const JobDescription& desc = getDescription();
	SolverSetup setup;
	setup.logger = &Logger::getMainInstance();
	setup.jobname = "shweep-"+to_string(_my_index);
	setup.numVars = desc.getAppConfiguration().fixedSizeEntryToInt("__NV");
	setup.numOriginalClauses = desc.getAppConfiguration().fixedSizeEntryToInt("__NC");
	setup.localId = localId;
	setup.globalId = _my_rank * _params.numThreadsPerProcess.val + localId;

	if (_params.satProfilingLevel() >= 0) {
		setup.profilingBaseDir = _params.satProfilingDir();
		if (setup.profilingBaseDir.empty()) setup.profilingBaseDir = TmpDir::getGeneralTmpDir();
		setup.profilingBaseDir += "/" + std::to_string(_my_rank) + "/"; // rank == appRank ?
		LOG(V4_VVER, "SWEEP [%i](%i) Profiling Dir = %s \n", _my_rank, localId, setup.profilingBaseDir.c_str());
		FileUtils::mkdir(setup.profilingBaseDir);
		setup.profilingLevel = _params.satProfilingLevel();
	}


	if (_numVars==0)
		_numVars = setup.numVars;

	// LOG(V2_INFO, "SWEEP JOB [%i](%i) create kissat shweeper \n", _my_rank, localId);
	float t0 = Timer::elapsedSeconds();
	std::shared_ptr<Kissat> shweeper(new Kissat(setup));
	float t1 = Timer::elapsedSeconds();
	LOG(V2_INFO, "SWEEP STARTUP [%i](%i) kissat init in %f ms\n", _my_rank, localId, (t1 - t0)*1000);
	if (_terminate_all) {
		LOG(V2_INFO, "SWEEP JOB TERMINATE [%i](%i) terminated new kissat directly, job already done \n", _my_rank, localId);
		return shweeper;
	}
	shweeper->setIsShweeper();

	shweeper->shweepSetImportExportCallbacks();
    shweep_set_search_work_callback(shweeper->solver, this, cb_search_work_in_tree); //here we connect directly between SweepJob and kissat-solver, bypassing Kissat::

	if (_is_root) {
		//read out final formula only at the root node
		shweeper->shweepSetReportCallback();
		shweeper->shweepSetDimacsReportPtr(_dimacsReport_localId);
	}

    //Basic configuration
    shweeper->set_option("quiet", 1);  //suppress any standard kissat messages
    shweeper->set_option("verbose", 0);//the native kissat verbosity
    // _shweeper->set_option("log", 0);//extensive logging
    shweeper->set_option("check", 0);  // do not check model or derived clauses, because we import anyways units and equivalences without proof tracking
    shweeper->set_option("statistics", 1);  //print full statistics
    shweeper->set_option("profile", _params.satProfilingLevel.val); // do detailed profiling how much time we spent where
	shweeper->set_option("seed", 0);   //Sweeping should not contain any RNG part

	//Specific due to Mallob
	shweeper->set_option("mallob_custom_sweep_verbosity", _params.sweepSolverVerbosity.val); //Shweeper verbosity 0..4
	shweeper->set_option("mallob_is_shweeper", 1); //Make this Kissat solver a pure Distributed Sweeping Solver. Jumps directly to distributed sweeping and bypasses everything else
	shweeper->set_option("mallob_local_id", localId);
	shweeper->set_option("mallob_rank", _my_rank);
	shweeper->set_option("mallob_is_root", _is_root);
	shweeper->set_option("mallob_resweep_chance", _params.sweepResweepChance.val);


	//Own options of Kissat
	shweeper->set_option("sweepcomplete", 1);      //deactivates checking for time limits during sweeping, so we dont get kicked out due to some limits
	//Specific for clean sweep run
	shweeper->set_option("preprocess", 0); //skip other preprocessing stuff after shweep finished
	// shweeper->set_option("probe", 1);   //there is some cleanup-probing at the end of the sweeping. keep it? (apparently the probe option is used nowhere anyways)
	shweeper->set_option("substitute", 1); //apply equivalence substitutions after sweeping (kissat default 1, but keep here explicitly to remember it)
	shweeper->set_option("substituterounds", 2); //default is 2, and changing that has currently no effect, because all substitutions happen in round 1, and already in round 2 zero substitutions are found, so it exits there.
	// shweeper->set_option("substituteeffort", 1000); //modification doesnt seem to have much effect...
	// shweeper->set_option("substituterounds", 10);
	shweeper->set_option("luckyearly", 0); //skip
	shweeper->set_option("luckylate", 0);  //skip

	return shweeper;
}

void SweepJob::readResult(KissatPtr shweeper) {
	_internal_result.id = getId();
	_internal_result.revision = getRevision();
	_internal_result.result= SAT; //technically its not SAT but just *some* information, but just calling it SAT helps to seamlessly pass it though the higher abstraction layers
	std::vector<int> formula = shweeper->extractPreprocessedFormula();
	_internal_result.setSolutionToSerialize(formula.data(), formula.size()); //Format: [Clauses, #Vars, #Clauses]

	//Logging
	auto stats = shweeper->getSolverStats();
	int units_orig = stats.shweep_total_units - stats.shweep_new_units;
	int total_orig = stats.shweep_active_orig + units_orig;
	int actual_done = stats.shweep_eqs + stats.shweep_sweep_units;
	//actual_done is a slightly conservative count, because we only include the units found by the sweeping algorithm itself,
	//and dont include some stray units found while propagating the sweep decisions (that would be stats.shweep_new_units)
	int actual_remaining = total_orig - actual_done;
	// int actual_eliminated ;

	printf("SWEEP finished\n");
	printf("[%i](%i) RESULT SWEEP: %i Eqs, %i sweep_units, %i new units, %i total units, %i eliminated \n",
		_my_rank, _dimacsReport_localId->load(), stats.shweep_eqs, stats.shweep_sweep_units, stats.shweep_new_units, stats.shweep_total_units, stats.shweep_eliminated);
	LOG(V2_INFO, "[%i](%i) RESULT SWEEP: %i Eqs, %i sweep_units, %i new units, %i total units, %i eliminated \n",
		_my_rank, _dimacsReport_localId->load(), stats.shweep_eqs, stats.shweep_sweep_units, stats.shweep_new_units, stats.shweep_total_units, stats.shweep_eliminated);
	LOG(V2_INFO, "[%i](%i) RESULT SWEEP: %i Processes, %f seconds \n", _my_rank, _dimacsReport_localId->load(), getVolume(), Timer::elapsedSeconds() - _start_shweep_timestamp);
	LOG(V2_INFO, "RESULT SWEEP_PRIORITY       %f\n", _params.preprocessSweepPriority.val);
	LOG(V2_INFO, "RESULT SWEEP_PROCESSES      %i\n", getVolume());
	LOG(V2_INFO, "RESULT SWEEP_THREADS_PP     %i\n", _params.numThreadsPerProcess.val);
	LOG(V2_INFO, "RESULT SWEEP_SHARING_PERIOD %i ms \n", _params.sweepSharingPeriod_ms.val);
	LOG(V2_INFO, "RESULT SWEEP_VARS_ORIG      %i\n", stats.shweep_vars_orig);
	LOG(V2_INFO, "RESULT SWEEP_VARS_END       %i\n", stats.shweep_vars_end);
	LOG(V2_INFO, "RESULT SWEEP_ACTIVE_ORIG    %i\n", stats.shweep_active_orig);
	LOG(V2_INFO, "RESULT SWEEP_ACTIVE_END     %i\n", stats.shweep_active_end);
	LOG(V2_INFO, "RESULT SWEEP_CLAUSES_ORIG   %i\n", stats.shweep_clauses_orig);
	LOG(V2_INFO, "RESULT SWEEP_CLAUSES_END    %i\n", stats.shweep_clauses_end);
	LOG(V2_INFO, "RESULT SWEEP_UNITS_ORIG     %i\n", units_orig);
	LOG(V2_INFO, "RESULT SWEEP_UNITS_NEW      %i\n", stats.shweep_new_units);
	LOG(V2_INFO, "RESULT SWEEP_UNITS_TOTAL    %i\n", stats.shweep_total_units);
	LOG(V2_INFO, "RESULT SWEEP_ELIMINATED     %i\n", stats.shweep_eliminated);
	LOG(V2_INFO, "RESULT SWEEP_EQUIVALENCES   %i\n", stats.shweep_eqs);
	LOG(V2_INFO, "RESULT SWEEP_UNITS_SWEEP    %i\n", stats.shweep_sweep_units);
	LOG(V2_INFO, "RESULT SWEEP_ACTUAL_DONE    %i / %i \n", actual_done, total_orig);
	LOG(V2_INFO, "RESULT SWEEP_ACTUAL_REMAIN  %i / %i \n", actual_remaining, total_orig);
	LOG(V2_INFO, "RESULT SWEEP_TIME           %f sec\n", Timer::elapsedSeconds() - _start_shweep_timestamp);

	for (int i=0; i < _sharing_start_ping_timestamps.size() && i < _sharing_receive_result_timestamps.size(); i++) {
		float start = _sharing_start_ping_timestamps[i];
		float end = _sharing_receive_result_timestamps[i];
		LOG(V2_INFO, "RESULT SWEEP_SHARING_LATENCY  %f ms   (ping->result  %f --> %f) \n", (end-start)*1000, start, end);
	}
	for (int i=0; i < _sharing_start_ping_timestamps.size() -1; i++) {
		LOG(V2_INFO, "RESULT SWEEP_SHARING_PERIOD_REAL  %f ms \n", (_sharing_start_ping_timestamps[i+1] - _sharing_start_ping_timestamps[i])*1000);
	}

	LOG(V3_VERB, "# # RESULT [%i](%i) Serialized final formula, SolutionSize=%i\n", _my_rank, _dimacsReport_localId->load(), _internal_result.getSolutionSize());
	for (int i=0; i<15; i++) {
		LOG(V3_VERB, "RESULT Formula peek %i: %i \n", i, _internal_result.getSolution(i));
	}


	//This flag tells the system that the result is actually ready
	_solved_status = SAT;
}


// Called periodically by the main thread to allow the worker to emit messages.
void SweepJob::appl_communicate() {
	LOG(V4_VVER, "SWEEP JOB appl_communicate() \n");

	printIdleFraction();
	sendMPIWorkstealRequests();
	if (_bcast && _is_root)// Root: Update job tree snapshot in case your children changed
		_bcast->updateJobTree(getJobTree());

	if (_is_root)
		initiateNewSharingRound();

	advanceAllReduction();
}



// React to an incoming message. (This becomes relevant only if you send custom messages)
void SweepJob::appl_communicate(int sourceRank, int mpiTag, JobMessage& msg) {
	// LOG(V2_INFO, "Shweep rank %i: received custom message from source %i, mpiTag %i, msg.tag %i \n", _my_rank, source, mpiTag, msg.tag);
	if (msg.returnedToSender) {
		LOG(V0_CRIT, "SWEEP MSG Error [%i]: received unexpected returnedToSender message during Sweep Job Workstealing!\n", _my_rank);
		LOG(V0_CRIT, "SWEEP MSG Error [%i]: source=%i mpiTag=%i, msg.tag=%i treeIdxOfSender=%i, treeIdxOfDestination=%i \n", _my_rank, sourceRank, mpiTag, msg.tag, msg.treeIndexOfSender, msg.treeIndexOfDestination);
		assert(false);
	}
	if (msg.tag == TAG_SEARCHING_WORK) {
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
			log_return_false("SWEEP STEAL Error in TAG_RETURNING_STEAL_REQUEST! Want to return an message, but invalid contextIdOfDestination==0. "
					"With sourceRank=%i, sourceIndex=%i, payload.size()=%i \n", sourceRank, sourceIndex, msg.payload.size()));
		getJobTree().send(sourceRank, MSG_SEND_APPLICATION_MESSAGE, msg);
		return;
	}

	if (msg.tag == TAG_RETURNING_STEAL_REQUEST) {
		int localId = msg.payload.back();
		msg.payload.pop_back();
		_worksteal_requests[localId].stolen_work = std::move(msg.payload);
		_worksteal_requests[localId].got_steal_response = true;
		LOG(V3_VERB, "SWEEP MSG to [%i](%i) received steal answer --%i-- from [%i]\n", _my_rank, localId, _worksteal_requests[localId].stolen_work.size(), sourceRank );
		return;
	}
}



void SweepJob::printIdleFraction() {
	int idles = 0;
	int active = 0;
	std::ostringstream oss;
	for (auto shweeper : _shweepers) {
		if (shweeper) {
			active++;
			if (shweeper->shweeper_is_idle) {
				idles++;
				oss << "(" << shweeper->getLocalId() << ") ";
			}
		}
	}
	LOG(V3_VERB, "SWEEP IDLE  %i/%i : %s \n", idles, active, oss.str().c_str());
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

			assert(msg.contextIdOfDestination != 0 || log_return_false("SWEEP Error: contextIdOfDestination==0 in workstealing request! Source rank=%i, targetRank %i \n", _my_rank, request.targetRank));
			msg.payload = {request.localId};
			// LOG(V2_INFO, "Rank %i asks rank %i for work\n", _my_rank, recv_rank, n);
			// LOG(V2_INFO, "  with destionation ctx_id %i \n", msg.contextIdOfDestination);
			LOG(V3_VERB, "SWEEP MSG sending MPI request [%i](%i) -> [%i] \n", _my_rank, request.localId, request.targetRank);
			getJobTree().send(request.targetRank, MSG_SEND_APPLICATION_MESSAGE, msg);
		}
	}
}

void SweepJob::cbSearchWorkInTree(unsigned **work, int *work_size, int localId) {
	KissatPtr shweeper = _shweepers[localId]; //array access safe (we know the sweeper exists) because this callback is called by this sweeper itself
	shweeper->shweeper_is_idle = true;
	shweeper->work_received_from_steal = {};

	//loop until we find work or the whole sweeping is terminated
	while (true) {
		if (_terminate_all) {
			//this is the signal for the solver to terminate itself, by sending it a work array of size 0
			shweeper->work_received_from_steal = {};
			break;
		}
		 /*
		  * At the root node we serve the initial work to whichever solver asks first
		  */
		if (_is_root && ! _root_provided_initial_work) {
			_root_provided_initial_work = true;
			//We need to know how much space to allocate to store each variable "idx" at the array position work[idx].
			//i.e. we need to know max(idx).
			//We assume that the maximum variable index corresponds to the total number of variables
			//i.e. that there are no holes in kissats internal numbering. This is an assumption that standard Kissat makes all the time, so we also do it here
			const unsigned VARS = shweep_get_num_vars(shweeper->solver); //this value can be different from numVars here in C++ !! Because kissat might havel aready propagated some units, etc.
			shweeper->work_received_from_steal = std::vector<int>(VARS);
			//the initial work is all variables
			for (int idx = 0; idx < VARS; idx++) {
				shweeper->work_received_from_steal[idx] = idx;
			}
			LOG(V2_INFO, "SWEEP WORK First shweeper at [%i](%i) got all %u variables\n", _my_rank, localId, VARS);
			break;

		}

		//Try to steal locally from shared memory

		// int rnd_percent = _rng.randomInRange(0,100);
		// LOG(V2_INFO, "SWEEP STEAL [%i](%i) rnd_percent = %i \n", _my_rank, localId, rnd_percent);
		// bool first_local =  rnd_percent <= SEARCH_FIRST_LOCAL_PERCENT;

		 /*
		  * Going for some direct global steals doesnt do much here, because the big rally happens anyways the moment all are depleted locally
		  * At that point they anyways schedule a global sweep, and some sweepers stealing globally before cant stop that
		  */

		// if (first_local) {
		auto stolen_work = stealWorkFromAnyLocalSolver();
		//Successful local steal
		if ( ! stolen_work.empty()) {
			//store steal data persistently in C++, such that C can keep operating on that memory segment
			shweeper->work_received_from_steal = std::move(stolen_work);
			LOG(V3_VERB, "SWEEP WORK [%i] ---%i---> (%i) \n", _my_rank, shweeper->work_received_from_steal.size(), localId);
			break;
		}
		// } else {
			// LOG(V2_INFO, "SWEEP STEAL [%i](%i) go immediately for global steal\n", _my_rank, localId);
		// }

		int my_comm_rank = getJobComm().getWorldRankOrMinusOne(_my_index);

		if (my_comm_rank == -1) {
			LOG(V3_VERB, "SWEEP SKIP Delaying global steal request, my own rank %i (index %i) is not yet in JobComm \n", _my_rank, _my_index);
			// LOG(V2_INFO, " with _my_index %i \n", _my_index);
			// LOG(V2_INFO, " with JobComm().size %i \n", getJobComm().size());
			usleep(1000);
			continue;
		}

		//Unsuccessful steal locally. Go global via MPI message
		int targetIndex = _rng.randomInRange(0,getVolume());
        int targetRank = getJobComm().getWorldRankOrMinusOne(targetIndex);


        if (targetRank == -1) {
        	//target rank not yet in JobTree, might need some more milliseconds to update, try again
			LOG(V3_VERB, "SWEEP SKIP targetIndex %i, targetRank %i not yet in JobComm\n", targetIndex, targetRank);
			usleep(1000);
        	continue;
        }

		if (targetRank == _my_rank) {
			// not stealing from ourselves, try again
			continue;
		}

		if (getJobComm().getContextIdOrZero(targetIndex)==0) {
			LOG(V3_VERB, "SWEEP SKIP Context ID of target is missing. getVolume()=%i, rndTargetIndex=%i, rndTargetRank=%i, myIndex=%i, myRank=%i \n", getVolume(), targetIndex, targetRank, _my_index, _my_rank);
			//target is not yet listed in address list. Might happen for a short period just after it is spawned
			usleep(1000);
			continue;
		}

		// LOG(V3_VERB, "SWEEP Global steal request to targetIndex %i, targetRank=%i \n", targetIndex, targetRank);

		//Request will be handled by the MPI main thread, which will send an MPI message on our behalf
		//because here we are in code executed by the kissat thread, which can cause problems for sending MPI messages
		WorkstealRequest request;
		request.localId = localId;
		request.targetIndex = targetIndex;
		request.targetRank = targetRank;
		_worksteal_requests[localId] = request;
		// LOG(V3_VERB, "SWEEP  [%i](%i) searches work globally\n", _my_rank, localId);

		//Wait here until we get back an MPI message
		while( ! _worksteal_requests[localId].got_steal_response) {
			usleep(100);
		}

		//Successful steal if size > 0
		if ( ! _worksteal_requests[localId].stolen_work.empty()) {
			shweeper->work_received_from_steal = std::move(_worksteal_requests[localId].stolen_work);
			LOG(V3_VERB, "SWEEP WORK via MPI [%i] ---%i---> [%i](%i) \n", targetRank, shweeper->work_received_from_steal.size(), _my_rank, localId);
			break;
		}
		//Unsuccessful global steal, try again
	}
	//Found work (or terminated), Tell the kissat/C thread where it can find the work
	*work = reinterpret_cast<unsigned int*>(shweeper->work_received_from_steal.data());
	*work_size = shweeper->work_received_from_steal.size();
	shweeper->shweeper_is_idle = false;

	//The thread now returns to the kissat solver
	//work_size==0 set here is there interpreted as the termination signal
}


void SweepJob::initiateNewSharingRound() {
	if (!_bcast) {
		LOG(V1_WARN, "[WARN] SWEEP SHARE BCAST root couldn't initiate sharing round, _bcast is Null\n");
		return;
	}

	if (Timer::elapsedSeconds() < _last_sharing_start_timestamp + _params.sweepSharingPeriod_ms.val/1000.0) //convert to seconds
		return;


	if (_bcast->hasReceivedBroadcast()) {
		LOG(V1_WARN, "[WARN] SWEEP SHARE BCAST: Would like to initiate new sharing round, but old round is not completed yet\n");
		return;
	}
	//Broadcast a ping to all workers to initiate an AllReduce
	//The broadcast includes all workers currently reachable by the root-node and informs them about their parent and potential children
	//It then causes the leaf nodes to call the callback, initiating the AllReduce
	_last_sharing_start_timestamp = Timer::elapsedSeconds();
	_sharing_start_ping_timestamps.push_back(_last_sharing_start_timestamp);
	LOG(V3_VERB, "SWEEP SHARE BCAST Initiating Sharing via Ping\n");
	//todo: maybe reset bcast here, to prevent initiating with the same object twice, maybe prevent pingpong?
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

	if (! _is_root) {
		LOG(V4_VVER, "SWEEP SHARE [%i] RESET non-root BCAST\n", _my_rank);
		_bcast.reset(new JobTreeBroadcast(getId(), getJobTree().getSnapshot(),
			[this]() {cbContributeToAllReduce();}, TAG_BCAST_INIT));
		//root is reset only after the whole reduction result is broadcasted, to prevent starting a new one while the old one is still running
		//(might change this to add overlapping broadcasts later for faster turnovers, but for now keep only one for cleaner debugging)
	}


	JobMessage baseMsg = getMessageTemplate();
	baseMsg.tag = TAG_ALLRED;
	_red.reset(new JobTreeAllReduction(snapshot, baseMsg, std::vector<int>(), aggregateEqUnitContributions));


	//Bring individual data per thread in the sharing element format: [Equivalences, Units, eq_size, unit_size, all_idle]
	std::list<std::vector<int>> contribs;
	int id=-1; //for debugging
	for (auto &shweeper : _shweepers) {
		id++;
		if (!shweeper) {
			LOG(V4_VVER, "SWEEP SHARE [%i](%i) not yet initialized, skipped in contribution aggregation \n", _my_rank, id);
			continue;
		}
		int eq_size = shweeper->eqs_to_share.size();
		int units_size = shweeper->units_to_share.size();
		std::vector<int> contrib = std::move(shweeper->eqs_to_share);
		contrib.insert(contrib.end(), shweeper->units_to_share.begin(), shweeper->units_to_share.end());
		contrib.push_back(eq_size);
		contrib.push_back(units_size);
		contrib.push_back(shweeper->shweeper_is_idle);

		contribs.push_back(contrib);
		LOG(V3_VERB, "SWEEP SHARE REDUCE (%i): %i equivalences, %i units, %i idle \n", shweeper->getLocalId(), eq_size/2, units_size, shweeper->shweeper_is_idle);

		shweeper->units_to_share.clear();
		shweeper->eqs_to_share.clear();
	}

	// LOG(V3_VERB, "SWEEP Aggregate contributions within process\n");
	auto aggregation_element = aggregateEqUnitContributions(contribs);

	LOG(V3_VERB, "SWEEP SHARE REDUCE [%i]: size %i to sharing\n", _my_rank, aggregation_element.size()-NUM_SHARING_METADATA);
	_red->contribute(std::move(aggregation_element));

}

void SweepJob::advanceAllReduction() {
	if (!_red) return;
	LOG(V3_VERB, "SWEEP SHARE REDUCE ADVANCE [%i]\n", _my_rank);
	_red->advance();
	if (!_red->hasResult()) return;

	// LOG(V3_VERB, "[sweep] all-reduction complete\n");

	//Extract, unserialize and distribute shared Equivalences and units
	_sharing_receive_result_timestamps.push_back(Timer::elapsedSeconds());
	auto shared = _red->extractResult();
	const int eq_size = shared[shared.size()-EQUIVS_METADATA_POS];
	const int unit_size = shared[shared.size()-UNITS_METADATA_POS];
	const int all_idle = shared[shared.size()-IDLE_METADATA_POS];
	LOG(V3_VERB, "SWEEP SHARE REDUCE --- Received sharing data: %i equivalences, %i units -- \n", eq_size/2, unit_size);
	LOG(V3_VERB, "SWEEP SHARE REDUCE IDLE ALL %i \n", all_idle);
	if (all_idle) {
		_terminate_all = true;
		LOG(V1_WARN, "ß # \n # \n # --- SWEEP ALL SWEEPERS IDLE - CAN TERMINATE -- \n # \n");
	}

	_eqs_from_broadcast.assign(shared.begin(),             shared.begin() + eq_size);
	_units_from_broadcast.assign(shared.begin() + eq_size, shared.end() - NUM_SHARING_METADATA);
	// _eqs_from_broadcast.insert(_eqs_from_broadcast.end(),	  shared.begin(),                     shared.begin() + eq_size);
	// _units_from_broadcast.insert(_units_from_broadcast.end(), shared.begin() + eq_size, shared.end() - NUM_SHARING_METADATA);

	//For convenience, we copy the received data into each solver individually.
	//This makes importing the E/U data into each thread easier and less cumbersome to code, at the cost of slightly more memory usage
	//For maximum memory efficiency one would have all kissat threads directly read from this one SweepJob's array
	//We write to a queue, to not mess with the specific memory allocation the thread is currently working on
	int id=-1; //for debugging
	for (auto shweeper : _shweepers) {
		id++;
		if (!shweeper) {
			LOG(V4_VVER, "[WARN] SWEEP SHARE REDUCE [%i](%i) not yet initialized, skipped importing results!\n", _my_rank, id);
			continue;
		}
		shweeper->eqs_from_broadcast_queued.insert(shweeper->eqs_from_broadcast_queued.end(), _eqs_from_broadcast.begin(), _eqs_from_broadcast.end());
		shweeper->units_from_broadcast_queued.insert(shweeper->units_from_broadcast_queued.end(), _units_from_broadcast.begin(), _units_from_broadcast.end());
	}

	//Now we can reset the root node, broadcast is finished and can prepare a new one
	if (_is_root) {
		LOG(V4_VVER, "SWEEP SHARE [%i] RESET root BCAST\n", _my_rank);
		_bcast.reset(new JobTreeBroadcast(getId(), getJobTree().getSnapshot(),
			[this]() {cbContributeToAllReduce();}, TAG_BCAST_INIT));
	}

	LOG(V4_VVER, "SWEEP SHARE [%i] RESET REDUCE\n", _my_rank);
	_red.reset();
}



std::vector<int> SweepJob::aggregateEqUnitContributions(std::list<std::vector<int>> &contribs) {
	//Each contribution has the format [Equivalences,Units,eq_size,unit_size,all_idle].

	size_t total_size = NUM_SHARING_METADATA;
    for (const auto& contrib : contribs) {
	    total_size += contrib.size()-NUM_SHARING_METADATA;
    }
    std::vector<int> aggregated;
    aggregated.reserve(total_size);
	//Fill equivalences
	size_t total_eq_size = 0;
    for (const auto &contrib : contribs) {
    	int eq_size = contrib[contrib.size()-EQUIVS_METADATA_POS];
    	total_eq_size += eq_size;
		// LOG(V3_VERB, "ß Element: %i eq_size \n", eq_size);
        aggregated.insert(aggregated.end(), contrib.begin(), contrib.begin()+eq_size);
    }
	//Fill units
	size_t total_unit_size = 0;
    for (const auto &contrib : contribs) {
    	int eq_size = contrib[contrib.size()-EQUIVS_METADATA_POS];
    	int unit_size = contrib[contrib.size()-UNITS_METADATA_POS];
		total_unit_size += unit_size;
		// LOG(V3_VERB, "ß Element: %i unit_size \n", unit_size);
        aggregated.insert(aggregated.end(), contrib.begin()+eq_size, contrib.end()-NUM_SHARING_METADATA); //not copying the metadata at the end
    }
	//See whether all solvers are idle
	bool all_idle = true;
    for (const auto &contrib : contribs) {
		bool idle = contrib[contrib.size()-IDLE_METADATA_POS];
    	all_idle &= idle;
		// LOG(V3_VERB, "ß Element: idle == %i \n", idle);
    }
	if (contribs.empty()) {
		all_idle = false; //edge-case: not a single solver is initialized yet, we are waiting for them to come online, they are not idle
	}


	aggregated.push_back(total_eq_size);
	aggregated.push_back(total_unit_size);
	aggregated.push_back(all_idle);
	LOG(V3_VERB, "SWEEP SHARE REDUCE aggregated %i equivalences, %i units, %i all_idle\n", total_eq_size/2, total_unit_size, all_idle);
	assert(total_size == total_eq_size + total_unit_size + NUM_SHARING_METADATA);
    return aggregated;
}



std::vector<int> SweepJob::stealWorkFromAnyLocalSolver() {
	auto rand_permutation = getRandomIdPermutation();

	std::ostringstream oss;
	oss << "Steal permutation: ";
	for (int id : rand_permutation) {
		oss << id << ' ';
	}
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
	if ( ! _shweepers[localId]) {
		// LOG(V3_VERB, "SWEEP STEAL stealing from [%i](%i), shweeper does not exist yet\n", _my_rank, localId);
		return {};
	}
	KissatPtr shweeper = _shweepers[localId];
	if ( ! shweeper->solver) {
		// LOG(V3_VERB, "SWEEP STEAL stealing from [%i](%i), shweeper->solver does not exist yet \n", _my_rank, localId);
		return {};
	}

	//We dont know yet how much there is to steal, so we ask for an upper bound
	//It can also be that the solver we want to steal from is not fully initialized yet
	//For that in the C code there are further guards against unfinished initialization, all returning 0 in that case
	// LOG(V3_VERB, "SWEEP STEAL [%i] getting max steal info from (%i) \n", _my_rank, localId);
	int max_steal_amount = shweep_get_max_steal_amount(shweeper->solver);
	if (max_steal_amount == 0)
		return {};

	// LOG(V2_INFO, "ß %i max_steal_amount\n", max_steal_amount);
	assert(max_steal_amount > 0 || log_return_false("SWEEP STEAL Error [%i](%i): negative max steal amount %i, maybe segfault into non-initialized kissat solver \n", _my_rank, localId, max_steal_amount));
	assert(max_steal_amount < 2*_numVars || log_return_false("SWEEP STEAL Error [%i](%i): too large max steal amount %i >= 2*NUM_VARS, maybe segfault into non-initialized kissat solver \n", _my_rank, localId, max_steal_amount));

	//There is something to steal
	//Allocate memory for the steal here in C++, and pass the array location to kissat such that it can fill it with the stolen work
	std::vector<int> stolen_work = std::vector<int>(max_steal_amount);

	// LOG(V3_VERB, "[%i] stealing from (%i), expecting max %i  \n", _my_rank, localId, max_steal_amount);
	int actually_stolen = shweep_steal_from_this_solver(shweeper->solver, reinterpret_cast<unsigned int*>(stolen_work.data()), max_steal_amount);
	// LOG(V3_VERB, "ß Steal request got %i actually stolen\n", actually_stolen);
	if (actually_stolen == 0)
		return {};
	//We sized he provided array to be maximally conservative,
	//Now we learned how much there way actually to steal, shrink the array to have .size() match with the stolen amount
	stolen_work.resize(actually_stolen);
	return stolen_work;
}

std::vector<int> SweepJob::getRandomIdPermutation() {
	auto permutation = _list_of_ids; //copy
	static thread_local std::mt19937 rng(std::random_device{}()); //created/seeded once per thread, then only advancing calls
	std::shuffle(permutation.begin(), permutation.end(), rng);
	return permutation;
}


void SweepJob::loadFormula(KissatPtr shweeper) {
	const int* lits = getDescription().getFormulaPayload(0);
	const int payload_size = getDescription().getFormulaPayloadSize(0);
	// LOG(V2_INFO, "SWEEP Loading Formula, size %i \n", payload_size);
	for (int i = 0; i < payload_size ; i++) {
		shweeper->addLiteral(lits[i]);
	}
}

SweepJob::~SweepJob() {
	for (auto &bg_worker : _bg_workers) {
		if (bg_worker->isRunning()) {
			bg_worker->stop();
		}
	}
	LOG(V3_VERB, "SWEEP JOB ENDING WORKERS\n");
}














