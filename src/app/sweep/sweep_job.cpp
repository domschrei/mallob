
#include "sweep_job.hpp"

#include "app/job.hpp"
#include "app/job_tree.hpp"
#include "util/ctre.hpp"
#include "util/logger.hpp"


SweepJob::SweepJob(const Parameters& params, const JobSetup& setup, AppMessageTable& table)
    : Job(params, setup, table)
{
	assert(_params.jobCommUpdatePeriod() > 0 || log_return_false("[ERROR] For this application to work,"
            " you must explicitly enable job communicators with the -jcup option, e.g., -jcup=0.1\n"));
	LOG(V2_INFO, "New SweepJob MPI Process rank %i with %i threads\n", getJobTree().getRank(), params.numThreadsPerProcess.val);
}


void search_work_in_tree(void *SweepJob_state, unsigned **work, int *work_size, int local_id) {
    ((SweepJob*) SweepJob_state)->searchWorkInTree(work, work_size, local_id);
}


void SweepJob::appl_start() {
	_my_rank = getJobTree().getRank();
	_my_index = getJobTree().getIndex();
	_is_root = getJobTree().isRoot();
	LOG(V2_INFO,"ß SweepJob application start: Rank %i, Index %i, is root? %i, Parent-Index %i, \n",   _my_rank, _my_index, _is_root, getJobTree().getParentIndex());
	LOG(V2_INFO,"ß							 : num children %i\n", getJobTree().getNumChildren());
    _metadata = getSerializedDescription(0)->data();
	_last_sharing_timestamp = Timer::elapsedSeconds();

	//do not trigger a send on the initial dummy worksteal requests
	_worksteal_requests.resize(_params.numThreadsPerProcess.val);
	for (auto &request : _worksteal_requests) {
		request.sent = true;
	}

	//IDs that will be shuffled for each workstealing request
	for (int localId=0; localId < _params.numThreadsPerProcess.val; ++localId) {
		_list_of_ids.push_back(localId);
	}


	//this broadcast object is used to "ping" the processes currently in the tree reachable by the root node
	//the ping provides them a callback, where they contribute their local element to the allreduction
	LOG(V2_INFO, "[sweep] initialize broadcast object\n");
	_bcast.reset(new JobTreeBroadcast(getId(), getJobTree().getSnapshot(),
		[this]() {contributeToAllReduceCallback();}, BCAST_INIT));


	// LOG(V2_INFO,"ß Initializing baseMsg\n");
	// JobMessage baseMsg = getMessageTemplate();
	// baseMsg.tag = ALLRED;
	// _red.reset(new JobTreeAllReduction(getJobTree().getSnapshot(), baseMsg, std::vector<int>(), aggregateContributions));
	// _red->setCareAboutParent();

	//Starting individual threads
	for (int localId=0; localId < _params.numThreadsPerProcess.val; localId++) {
		auto shweeper = createNewShweeper(localId);
		_shweepers.push_back(shweeper);
		startShweeper(shweeper);
	}

	LOG(V2_INFO, "ß Finished SweepJob appl_start() \n");
}

std::shared_ptr<Kissat> SweepJob::createNewShweeper(int localId) {
	// LOG(V2_INFO,"ß Creating new shweeper %i \n", localId);
	const JobDescription& desc = getDescription();
	SolverSetup setup;
	setup.logger = &Logger::getMainInstance();
	setup.jobname = "shweep-"+to_string(_my_index);
	setup.numVars = desc.getAppConfiguration().fixedSizeEntryToInt("__NV");
	setup.numOriginalClauses = desc.getAppConfiguration().fixedSizeEntryToInt("__NC");
	setup.localId = localId;

	std::shared_ptr<Kissat> shweeper(new Kissat(setup));
	shweeper->set_option("mallob_custom_sweep_verbosity", 2); //Shweeper verbosity 0..4
	// shweeper->set_option("mallob_solver_count", NUM_WORKERS);
	shweeper->set_option("mallob_local_id", localId);
	shweeper->set_option("mallob_rank", _my_rank);

	shweeper->shweepSetImportExportCallbacks();
	shweeper->shweepSetWorkstealingCallback(this, &search_work_in_tree);
	shweeper->shweepSetReportCallback(); //for kissat_report_dimacs

    // Basic configuration options for all solvers
    shweeper->set_option("quiet", 1); // suppress any standard kissat output
    shweeper->set_option("verbose", 0); //the native kissat verbosity
    // _shweeper->set_option("log", 0); //extensive logging
    shweeper->set_option("check", 0); // do not check model or derived clauses
    shweeper->set_option("profile",3); // do detailed profiling how much time we spent where
	shweeper->set_option("seed", 0);   //keep seeds identical and constant for now, for easier debugging

	shweeper->set_option("mallob_is_shweeper", 1); //Make this Kissat solver a pure Distributed Sweeping Solver. Jumps directly to distributed sweeping and bypasses everything else
	shweeper->set_option("sweepcomplete", 1);      //full sweeping, deactivates any tick limits

	//Skip everything that lies between direct Sweeping and formula reporting
	shweeper->set_option("preprocess", 0);
	// shweeper->set_option("probe", 1);      //there is some cleanup-probing at the end of the sweeping, keep it?
	shweeper->set_option("luckyearly", 0);
	shweeper->set_option("luckylate", 0);

	return shweeper;
}

void SweepJob::startShweeper(KissatPtr shweeper) {
	// LOG(V2_INFO,"ß Calling new thread\n");
	std::future<void> fut_shweeper = ProcessWideThreadPool::get().addTask([this, shweeper]() { //Changed [&] to [this, shweeper]!! (nicco)
		// LOG(V2_INFO, "Start Thread (r %i, id %i)\n", _my_rank, shweeper->getLocalId());
		_running_shweepers_count++;
		loadFormula(shweeper);

		// LOG(V2_INFO, "shweeper->solve() (r %i, id %i)\n", _my_rank, shweeper->getLocalId());
		int res = shweeper->solve(0, nullptr);
		LOG(V2_INFO, "# # Thread finished. Rank %i localId %i result %i # #\n", _my_rank, shweeper->getLocalId(), res);
		_internal_result.id = getId();
		_internal_result.revision = getRevision();
		_internal_result.result=res;
		_solved_status = 0;
		_internal_result.setSolution(shweeper->extractPreprocessedFormula());
		// _internal_result.setSolutionToSerialize((int*)(dummy_solution.data()), dummy_solution.size());
		_running_shweepers_count--;
	});
	_fut_shweepers.push_back(std::move(fut_shweeper));
}


// Called periodically by the main thread to allow the worker to emit messages.
void SweepJob::appl_communicate() {

	sendMPIWorkstealRequests();

	// Root: Update job tree snapshot in case your children changed
	if (_bcast && _is_root)
		_bcast->updateJobTree(getJobTree());

	if (_is_root)
		initiateNewSharingRound();

	advanceAllReduction();

}



// React to an incoming message. (This becomes relevant only if you send custom messages)
void SweepJob::appl_communicate(int source, int mpiTag, JobMessage& msg) {
	// LOG(V2_INFO, "Shweep rank %i: received custom message from source %i, mpiTag %i, msg.tag %i \n", _my_rank, source, mpiTag, msg.tag);
	if (msg.tag == TAG_SEARCHING_WORK) {
		assert(msg.payload.size() == 1);
		int localId = msg.payload.front();
		msg.payload.clear();

		LOG(V2_INFO, "ß Received MPI steal request from [%i](%i) \n", source, localId);
		auto locally_stolen_work = stealWorkFromAnyLocalSolver();

		msg.payload = std::move(locally_stolen_work);
		msg.payload.push_back(localId);

		//send back to source
		msg.tag = TAG_RETURNING_STEAL_REQUEST;
		msg.treeIndexOfDestination = source;
		msg.contextIdOfDestination = getJobComm().getContextIdOrZero(source);
		getJobTree().send(source, MSG_SEND_APPLICATION_MESSAGE, msg);
		return;
	}

	if (msg.tag == TAG_RETURNING_STEAL_REQUEST) {
		int localId = msg.payload.back();
		msg.payload.pop_back();
		_worksteal_requests[localId].stolen_work = std::move(msg.payload);
		_worksteal_requests[localId].got_steal_response = true;
		return;
	}
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
			msg.treeIndexOfDestination = request.targetRank;
			msg.contextIdOfDestination = getJobComm().getContextIdOrZero(request.targetRank);
			msg.payload = {request.localId};
			// LOG(V2_INFO, "Rank %i asks rank %i for work\n", _my_rank, recv_rank, n);
			// LOG(V2_INFO, "  with destionation ctx_id %i \n", msg.contextIdOfDestination);
			LOG(V2_INFO, "  MPI work request from [%i](%i) to [%i] \n", _my_rank, request.localId, request.targetRank);
			getJobTree().send(request.targetRank, MSG_SEND_APPLICATION_MESSAGE, msg);
		}
	}
}

void SweepJob::searchWorkInTree(unsigned **work, int *work_size, int localId) {
	KissatPtr shweeper = _shweepers[localId];
	shweeper->shweeper_is_idle = true;
	shweeper->work_received_from_steal = {};

	SplitMix64Rng _rng;
	while (true) {
		if (_terminate_all) {
			//this sends kissat a size==0 array, which tells kissat that we terminate
			shweeper->work_received_from_steal = {};
			break;
		}

		if (_my_rank == 0 && localId == 0 && !_root_received_work) {
			//Give all the work to the first solver.
			//To know how much space we need to allocate for all variables, we assume that the maximum index of any variable corresponds to the total number of variables -1,
			//i.e. that there are no holes in the numbering.
			//This is an assumption that standard Kissat makes all the time, so we also do it here
			const unsigned VARS = shweep_get_num_vars(shweeper->solver); //can be different than numVars here in C++ !! For example instant units are already propagated out.
			shweeper->work_received_from_steal = std::vector<int>(VARS);
			//the initial work is all variables
			for (int idx = 0; idx < VARS; idx++) {
				shweeper->work_received_from_steal[idx] = idx;
			}
			_root_received_work = true;
			LOG(V2_INFO, "Initial work: Shweep root [%i](%i) requested work, got all %u variables\n", _my_rank, localId, VARS);
			break;

		}

		//Try to steal locally from shared memory
		LOG(V2_INFO, "  [%i](%i) searches work locally \n", _my_rank, localId);
		auto stolen_work = stealWorkFromAnyLocalSolver();

		//Successful local steal
		if ( ! stolen_work.empty()) {
			//store steal data persistently in C++, such that C can keep operating on that memory segment
			shweeper->work_received_from_steal = std::move(stolen_work);
			LOG(V2_INFO, "%i variables sent within [%i] to (%i) \n", shweeper->work_received_from_steal.size(), _my_rank, localId);
			break;
		}

		//Unsuccessful steal locally. Go global via MPI message
		int recvIndex = _rng.randomInRange(0,getVolume());
        int targetRank = getJobComm().getWorldRankOrMinusOne(recvIndex);
        if (targetRank == -1) { // tree not fully built yet, try again
			usleep(100);
        	continue;
        }
		if (targetRank == _my_rank) { // don't steal from ourselves, try again
			continue;
		}

		//Request will be handled by the MPI main thread, which will send an MPI message on our behalf
		//because here we are in code executed by the kissat thread, which can cause problems for sending MPI messages
		WorkstealRequest request;
		request.localId = localId;
		request.targetRank = targetRank;
		_worksteal_requests[localId] = request;
		LOG(V2_INFO, "  [%i](%i) searches work globally\n", _my_rank, localId);

		//Wait here until we get back an MPI message
		while( ! _worksteal_requests[localId].got_steal_response) {
			usleep(100);
		}

		//Successful steal if size > 0
		if ( ! _worksteal_requests[localId].stolen_work.empty()) {
			shweeper->work_received_from_steal = std::move(_worksteal_requests[localId].stolen_work);
			LOG(V2_INFO, "%i variables received by [%i](%i) sent from [%i] \n", shweeper->work_received_from_steal.size(), _my_rank, localId,targetRank);
			break;
		}
		//Unsuccessful global steal, try again
	}
	//Found work (or terminated), Tell the kissat/C thread where it can find the work
	*work = reinterpret_cast<unsigned int*>(shweeper->work_received_from_steal.data());
	*work_size = shweeper->work_received_from_steal.size();
	shweeper->shweeper_is_idle = false;
}


void SweepJob::initiateNewSharingRound() {
	if (!_bcast) return;

	LOG(V2_INFO, "time %f\n", Timer::elapsedSeconds());
	LOG(V2_INFO, "last %f\n", _last_sharing_timestamp);
	LOG(V2_INFO, "l+p  %f\n", _last_sharing_timestamp + _params.sweepSharingPeriod_ms.val/1000.0);

	if (Timer::elapsedSeconds() < _last_sharing_timestamp + _params.sweepSharingPeriod_ms.val/1000.0)
		return;

	_last_sharing_timestamp = Timer::elapsedSeconds();
	//Broadcast a ping to all workers to initiate an AllReduce
	//The broadcast includes all workers currently reachable by the root-node (i.e. active) and informs them about their number of children in the current tree
	//It then causes the leaf nodes to call the callback, initiating the AllReduce
	LOG(V2_INFO, "Initiating Sharing via Ping\n");
	JobMessage msg = getMessageTemplate();
	msg.tag = _bcast->getMessageTag();
	msg.payload = {};
	_bcast->broadcast(std::move(msg));
}

void SweepJob::contributeToAllReduceCallback() {
	assert(_bcast);
	assert(_bcast->hasResult());

	auto snapshot = _bcast->getJobTreeSnapshot();

	_bcast.reset(new JobTreeBroadcast(getId(), getJobTree().getSnapshot(),
		[this]() {contributeToAllReduceCallback();}, BCAST_INIT));


	JobMessage baseMsg = getMessageTemplate();
	baseMsg.tag = ALLRED;
	_red.reset(new JobTreeAllReduction(snapshot, baseMsg, std::vector<int>(), aggregateEqUnitContributions));


	//Bring individual data per thread in the sharing element format: [Equivalences, Units, eq_size, unit_size, all_idle]
	std::list<std::vector<int>> contribs;
	for (auto &shweeper : _shweepers) {
		int eq_size = shweeper->eqs_to_share.size();
		int units_size = shweeper->units_to_share.size();
		std::vector<int> contrib = std::move(shweeper->eqs_to_share);
		contrib.insert(contrib.end(), shweeper->units_to_share.begin(), shweeper->units_to_share.end());
		contrib.push_back(eq_size);
		contrib.push_back(units_size);
		contrib.push_back(shweeper->shweeper_is_idle);

		contribs.push_back(contrib);
		LOG(V3_VERB, "ß New contribution: %i equivalences, %i units, %i idle \n", eq_size/2, units_size, shweeper->shweeper_is_idle);

		shweeper->units_to_share.clear();
		shweeper->eqs_to_share.clear();
	}

	LOG(V3_VERB, "ß Aggregate contributions within single process\n");
	auto aggregation_element = aggregateEqUnitContributions(contribs);

	LOG(V3_VERB, "ß contributing size %i to sharing\n", aggregation_element.size()-NUM_SHARING_METADATA);
	_red->contribute(std::move(aggregation_element));

}

void SweepJob::advanceAllReduction() {
	if (!_red) return;
	_red->advance();
	if (!_red->hasResult()) return;

	LOG(V2_INFO, "[sweep] all-reduction complete\n");

	//Extract, unserialize and distribute shared Equivalences and units
	auto shared = _red->extractResult();
	const int eq_size = shared[shared.size()-EQUIVS_SIZE_POS];
	const int unit_size = shared[shared.size()-UNITS_SIZE_POS];
	const int all_idle = shared[shared.size()-IDLE_STATUS_POS];
	LOG(V2_INFO, "ß --- Received sharing data: %i equivalences, %i units -- \n", eq_size/2, unit_size);
	if (all_idle) {
		_terminate_all = true;
		LOG(V1_WARN, "ß # \n # \n  --- ALL SWEEPERS IDLE - CAN TERMINATE -- \n # \n # \n");
	}

	_eqs_from_broadcast.assign(shared.begin(),             shared.begin() + eq_size);
	_units_from_broadcast.assign(shared.begin() + eq_size, shared.end() - NUM_SHARING_METADATA);
	// _eqs_from_broadcast.insert(_eqs_from_broadcast.end(),	  shared.begin(),                     shared.begin() + eq_size);
	// _units_from_broadcast.insert(_units_from_broadcast.end(), shared.begin() + eq_size, shared.end() - NUM_SHARING_METADATA);

	//For convenience, we copy the received data into each solver individually.
	//This makes importing the E/U data into each thread easier and less cumbersome to code, at the cost of slightly more memory usage
	//For maximum memory efficiency one would have all kissat threads directly read from this one SweepJob's array
	//We write to a queue, to not mess with the specific memory allocation the thread is currently working on
	for (auto shweeper : _shweepers) {
		shweeper->eqs_from_broadcast_queued.insert(shweeper->eqs_from_broadcast_queued.end(), _eqs_from_broadcast.begin(), _eqs_from_broadcast.end());
		shweeper->units_from_broadcast_queued.insert(shweeper->units_from_broadcast_queued.end(), _units_from_broadcast.begin(), _units_from_broadcast.end());
	}

	// Conclude the all-reduction
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
    	int eq_size = contrib[contrib.size()-EQUIVS_SIZE_POS];
    	total_eq_size += eq_size;
		// LOG(V3_VERB, "ß Element: %i eq_size \n", eq_size);
        aggregated.insert(aggregated.end(), contrib.begin(), contrib.begin()+eq_size);
    }
	//Fill units
	size_t total_unit_size = 0;
    for (const auto &contrib : contribs) {
    	int eq_size = contrib[contrib.size()-EQUIVS_SIZE_POS];
    	int unit_size = contrib[contrib.size()-UNITS_SIZE_POS];
		total_unit_size += unit_size;
		// LOG(V3_VERB, "ß Element: %i unit_size \n", unit_size);
        aggregated.insert(aggregated.end(), contrib.begin()+eq_size, contrib.end()-NUM_SHARING_METADATA); //not copying the metadata at the end
    }
	//See whether all solvers are idle
	bool all_idle = true;
    for (const auto &contrib : contribs) {
		bool idle = contrib[contrib.size()-IDLE_STATUS_POS];
    	all_idle &= idle;
		// LOG(V3_VERB, "ß Element: idle == %i \n", idle);
    }
	aggregated.push_back(total_eq_size);
	aggregated.push_back(total_unit_size);
	aggregated.push_back(all_idle);
	LOG(V3_VERB, "ß Aggregated: %i equivalences, %i units, %i all_idle\n", total_eq_size/2, total_unit_size, all_idle);
	assert(total_size == total_eq_size + total_unit_size + NUM_SHARING_METADATA);
    return aggregated;
}



std::vector<int> SweepJob::stealWorkFromAnyLocalSolver() {
	auto rand_permutation = getRandomIdPermutation(_running_shweepers_count);
	for (int localId : rand_permutation) {
		auto stolen_work = stealWorkFromSpecificLocalSolver(localId);
		if ( ! stolen_work.empty())
			return stolen_work;
	}
	//no work available, all local solvers are searching too
	return {};
}

std::vector<int> SweepJob::stealWorkFromSpecificLocalSolver(int localId) {
	if (_terminate_all) //job finished
		return {};
	if (_shweepers.size() < localId) //target solver not yet created
		return {};
	KissatPtr shweeper = _shweepers[localId];
	//We dont know how much there is to steal, so we ask for an upper bound
	//In the C code there are further guards against unfinished initialization of this particular thread, all returning 0
	int max_steal_amount = shweep_get_max_steal_amount(shweeper->solver);
	if (max_steal_amount == 0)
		return {};

	LOG(V2_INFO, "ß %i max_steal_amount\n", max_steal_amount);
	assert(max_steal_amount > 0);
	//There is something to steal
	//Allocate memory for the steal here in C++, and pass the array location to kissat such that it can fill it with the stolen work
	std::vector<int> stolen_work = std::vector<int>(max_steal_amount);
	int actually_stolen = shweep_steal_from_this_solver(shweeper->solver, reinterpret_cast<unsigned int*>(stolen_work.data()), max_steal_amount);
	LOG(V2_INFO, "ß Steal request got %i actually stolen\n", actually_stolen);
	if (actually_stolen == 0)
		return {};
	//We sized he provided array to be maximally conservative,
	//Now we learned how much there way actually to steal, shrink the array to have .size() match with the stolen amount
	stolen_work.resize(actually_stolen);
	return stolen_work;
}

std::vector<int> SweepJob::getRandomIdPermutation(int length) {
	auto permutation = _list_of_ids; //copy
	std::shuffle(permutation.begin(), permutation.end(), std::mt19937());
	return permutation;
}


// void SweepJob::importNextEquivalence(int *last_imported_round, int eq_nr, unsigned *lit1, unsigned *lit2) {
	// if (*last_imported_round == _sharing_round || eq_nr == _eqs_from_broadcast.size()/2) {
		//We already imported this round or we have just reached the end of importing this round
		// *lit1 = INVALID_LIT;
		// *lit2 = INVALID_LIT;
		// *last_imported_round = _sharing_round;
		// return;
	// }
	// *lit1 = _eqs_from_broadcast[2*eq_nr];
	// *lit2 = _eqs_from_broadcast[2*eq_nr + 1];
// }

void SweepJob::loadFormula(KissatPtr shweeper) {
	const int* lits = getDescription().getFormulaPayload(0);
	const int payload_size = getDescription().getFormulaPayloadSize(0);
	LOG(V2_INFO, "ß Loading Formula, size %i \n", payload_size);
	for (int i = 0; i < payload_size ; i++) {
		shweeper->addLiteral(lits[i]);
	}
}














