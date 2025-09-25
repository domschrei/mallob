
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
	_worksteal_requests.resize(params.numThreadsPerProcess.val);
	for (auto request : _worksteal_requests) {
		request.sent = true; //the initial dummy objects should not trigger a send
	}
	LOG(V2_INFO, "New SweepJob MPI Process with %i threads\n", params.numThreadsPerProcess.val);
}


void search_work_in_tree(void *SweepJob_state, unsigned **work, int *work_size, int local_id) {
	// LOG(V2_INFO, "Shweep %i search_work_in_tree \n", ((SweepJob*) SweepJob_state)->_my_rank);
    ((SweepJob*) SweepJob_state)->searchWorkInTree(work, work_size, local_id);
}



void SweepJob::appl_start() {
	_my_rank = getJobTree().getRank();
	_my_index = getJobTree().getIndex();
	_is_root = getJobTree().isRoot();
	printf("ß Appl_start(): Rank %i, Index %i, is root? %i, Parent-Index %i, \n",   _my_rank, _my_index, _is_root, getJobTree().getParentIndex());
	printf("ß			  : num children %i\n", getJobTree().getNumChildren());
    _metadata = getSerializedDescription(0)->data();

    auto permutations = AdjustablePermutation::getPermutations(3, 10);
	for (auto vec : permutations) {
		for (int i : vec) {
			std::cout << i << " ";
		}
		std::cout << std::endl;
	}

	//Initialize _red already here, to make sure that all processes have a valid reduction object
	//maybe switch back to more robust "standard" sharing
	JobMessage baseMsg = getMessageTemplate();
	baseMsg.tag = ALLRED;
	_red.reset(new JobTreeAllReduction(getJobTree().getSnapshot(), baseMsg, std::vector<int>(), aggregateContributions));
	_red->setCareAboutParent();


	for (int localId=0; localId < _params.numThreadsPerProcess.val; localId++) {
		auto shweeper = create_new_shweeper(localId);
		_shweepers.push_back(shweeper);
		startShweeper(shweeper);
	}

	LOG(V3_VERB, "ß Finished SweepJob::appl_start()\n");
}

std::shared_ptr<Kissat> SweepJob::create_new_shweeper(int localId) {

	const JobDescription& desc = getDescription();
	SolverSetup setup;
	setup.logger = &Logger::getMainInstance();
	setup.jobname = "shweep-"+to_string(_my_index);
	setup.numVars = desc.getAppConfiguration().fixedSizeEntryToInt("__NV");
	setup.numOriginalClauses = desc.getAppConfiguration().fixedSizeEntryToInt("__NC");
	setup.localId = localId;


	std::shared_ptr<Kissat> shweeper(new Kissat(setup));
	shweeper->set_option("mallob_custom_sweep_verbosity", 2); //0: No custom kissat messages. 1: Some. 2: More
	shweeper->set_option("mallob_solver_count", NUM_WORKERS);
	shweeper->set_option("mallob_local_id", localId);
	shweeper->set_option("mallob_rank", _my_rank);
	shweeper->shweep_set_importexport_callbacks();
	shweeper->shweep_set_workstealing_callback(this, &search_work_in_tree);

    // Basic configuration options for all solvers
    shweeper->set_option("quiet", 1); // suppress any standard kissat output
    shweeper->set_option("verbose", 0); //the native kissat verbosity
    // _shweeper->set_option("log", 0); //extensive logging
    shweeper->set_option("check", 0); // do not check model or derived clauses
    shweeper->set_option("profile",3); // do detailed profiling how much time we spent where
	shweeper->set_option("seed", _my_index);   //

	shweeper->set_option("mallob_is_shweeper", 1); //Make this Kissat solver a pure Distributed Sweeping Solver. Jumps directly to distributed sweeping and bypasses everything else
	shweeper->set_option("sweepcomplete", 1); //go for full sweeping, deactivates any tick limits
	shweeper->set_option("probe", 1); //there is some cleanup-probing at the end of the sweeping, keep it?
	return shweeper;
}

void SweepJob::startShweeper(KissatPtr shweeper) {
	std::future<void> fut_shweeper = ProcessWideThreadPool::get().addTask([&]() {
		LOG(V2_INFO, "Start Thread rank %i localId %i\n", _my_rank, shweeper->getLocalId());
		_shweepers_running_count++;
		loadFormula(shweeper);
		int res = shweeper->solve(0, nullptr);
		LOG(V2_INFO, "\n # \n # \n Thread finished. Rank %i localId %i result %i \n # \n # \n", _my_rank, shweeper->getLocalId(), res);
		_internal_result.id = getId();
		_internal_result.revision = getRevision();
		_internal_result.result=res;
		_solved_status = 10;
		auto dummy_solution = std::vector<int>(1,0);
		_internal_result.setSolutionToSerialize((int*)(dummy_solution.data()), dummy_solution.size());
		_shweepers_running_count--;
	});
	_fut_shweepers.push_back(fut_shweeper);
}


// Called periodically by the main thread to allow the worker to emit messages.
void SweepJob::appl_communicate() {

	// LOG(V3_VERB, "ß appl_communicate \n");
	double elapsed_time = Timer::elapsedSeconds();
	double wait_time = 0.001;
	bool can_start = elapsed_time > wait_time;


	//Worksteal requests need to be execute by the main MPI thread, because sending MPI messages via a callback from C can be problematic
	for (auto request : _worksteal_requests) {
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
			getJobTree().send(request.targetRank, MSG_SEND_APPLICATION_MESSAGE, msg);
		}
	}


	if (can_start && getVolume() == NUM_WORKERS && getJobComm().getWorldRankOrMinusOne(NUM_WORKERS-1) >= 0) {
		// LOG(V3_VERB, "ß appl_communicate. _red=%i \n", _red!=nullptr);
		// LOG(V3_VERB, "ß have %i eqs to share\n", _shweeper->eqs_to_share.size());
		// LOG(V3_VERB, "ß have %i units to share\n", _shweeper->units_to_share.size());
		bool reset_red = false;
		if (_red && _red->hasResult()) {
			auto broadcast = _red->extractResult(); //Broadcast data
			const int broadcasted_eq_size = broadcast[broadcast.size()-EQUIVS_SIZE_POS];
			const int broadcasted_unit_size = broadcast[broadcast.size()-UNITS_SIZE_POS];
			const int all_idle = broadcast[broadcast.size()-IDLE_STATUS_POS];
			LOG(V1_WARN, "ß --- Received Broadcast: %i eq_size, %i unit_size -- \n", broadcasted_eq_size, broadcasted_unit_size);
			if (all_idle) {
				_terminate_all = true;
				LOG(V1_WARN, "ß # \n # \n  --- ALL SWEEPERS IDLE - CAN TERMINATE -- \n # \n # \n");
			}

			_eqs_from_broadcast.assign(broadcast.begin(), broadcast.begin() + broadcasted_eq_size);
			_units_from_broadcast.assign(broadcast.begin() + broadcasted_eq_size, broadcast.end() - NUM_SHARING_METADATA);
			//save equivalences

			/**
			auto& eqs_received = _shweeper->eqs_received_from_sharing;
			eqs_received.reserve(eqs_received.size() + received_eq_size);
			eqs_received.insert(
				eqs_received.end(),
				std::make_move_iterator(broadcast_data.begin()),
				std::make_move_iterator(broadcast_data.begin() + received_eq_size)
			);
			//save units
			auto& units_received = _shweeper->units_received_from_sharing;
			units_received.reserve(units_received.size() + received_unit_size);
			units_received.insert(
				units_received.end(),
				std::make_move_iterator(broadcast_data.begin() + received_eq_size),
				std::make_move_iterator(broadcast_data.end()   - NUM_SHARING_METADATA)
			);
			**/
			reset_red = true;
			// parent_is_ready = _red->isParentReady();
			LOG(V3_VERB, "ß Now storing %i equivalences and %i units, for local solvers to import \n", _eqs_from_broadcast.size()/2, _units_from_broadcast.size());
		}

		if (!_started_sharing) { //triggers the very first construction
			reset_red = true;
			_started_sharing = true;
		}


		if (reset_red) {
			auto snapshot = getJobTree().getSnapshot();
			JobMessage baseMsg = getMessageTemplate();
			baseMsg.tag = ALLRED;
			bool parent_was_ready = _red->isParentReady();
			_red.reset(new JobTreeAllReduction(snapshot, baseMsg, std::vector<int>(), aggregateContributions));
			_red->setCareAboutParent();
			_red->tellChildrenParentIsReady();
			LOG(V3_VERB, "ß contributing %i eqs size\n", _shweeper->eqs_to_share.size());
			LOG(V3_VERB, "ß contributing %i units\n", _shweeper->units_to_share.size());
			LOG(V3_VERB, "ß contributing %i idle \n", _is_idle);
			//Combine Equivalences and Units in single array
			//also store how much space each takes, to quickly separate them without needing to search for the separator
			//for termination, also store a boolean whether all children are idling
			//Format: [Equivalences, Units, eq_size, unit_size, all_idle]
			const int eq_size = _shweeper->eqs_to_share.size();
			const int unit_size = _shweeper->units_to_share.size();
			std::vector<int> EU = std::move(_shweeper->eqs_to_share);
			EU.reserve(eq_size + unit_size + 2);
			std::move(_shweeper->units_to_share.begin(), _shweeper->units_to_share.end(), std::back_inserter(EU));
			EU.push_back(eq_size);
			EU.push_back(unit_size);
			EU.push_back(_is_idle);
			_shweeper->units_to_share.clear(); //because didn't move it!
			LOG(V3_VERB, "ß contributing in total %i size \n", EU.size());
			_red->contribute(std::move(EU));
			if (parent_was_ready) {
				_red->enableParentIsReady();
			}
		}
		_red->advance();
	}
}



// React to an incoming message. (This becomes relevant only if you send custom messages)
void SweepJob::appl_communicate(int source, int mpiTag, JobMessage& msg) {
	// LOG(V2_INFO, "Shweep rank %i: received custom message from source %i, mpiTag %i, msg.tag %i \n", _my_rank, source, mpiTag, msg.tag);
	if (msg.tag == TAG_SEARCHING_WORK) {
		assert(msg.payload.size() == 1);
		int localId = msg.payload.front();
		msg.payload.clear();

		auto stolen_work = stealWorkFromAnyLocalSolver();

		msg.payload = std::move(stolen_work);
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


void SweepJob::searchWorkInTree(unsigned **work, int *work_size, int localId) {
	_shweepers_idle_count++;
	KissatPtr shweeper = _shweepers[localId];
	shweeper->work_received_from_steal = {};

	//Basecase
	if (_my_rank == 0 && localId == 0 && !_root_received_work) {
	    //We assume that the maximum index of any variable corresponds to the total number of variables -1,
		//To know how much space we need to allocate for all variables.
	    //i.e. that there are no holes in the numbering. This is an assumption that standard Kissat makes all the time, so we also do it here
		unsigned VARS = shweep_get_num_vars(shweeper->solver);
		shweeper->work_received_from_steal = std::vector<int>(VARS);
		//the initial work: all variables
		for (int idx = 0; idx < VARS; idx++) {
			shweeper->work_received_from_steal[idx] = idx;
		}
		*work = reinterpret_cast<unsigned int*>(shweeper->work_received_from_steal.data());
		*work_size = VARS;
		_root_received_work = true;
		_shweepers_idle_count--;
		LOG(V2_INFO, "Initial work distribution: Shweep root %i,%i requested work, got all %u variables\n", _my_rank, localId, VARS);
		return;
	}

	//Actual stealing
	SplitMix64Rng _rng;
	while (true) {
		if (_terminate_all) {
			shweeper->work_received_from_steal = {};
			//this will terminate the kissat thread, because it will recognize size==0
			break;
		}

		//In each iteration try first to steal within the local process, without need for any MPI messages
		auto stolen_work = stealWorkFromAnyLocalSolver();

		//Successful local steal
		if ( ! stolen_work.empty()) {
			//store steal data persistently in C++, such that C can keep operating on that memory segment
			shweeper->work_received_from_steal = std::move(stolen_work);
			LOG(V2_INFO, "%i variables sent within same rank(%i) to id(%i) \n", shweeper->work_received_from_steal.size(), _my_rank, localId);
			break;
		}

		//Unsuccessful steal locally. Go global.
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

		//Wait here until we get back an MPI message
		while( ! _worksteal_requests[localId].got_steal_response) {
			usleep(100);
		}

		//Successful steal if size > 0
		if ( ! _worksteal_requests[localId].stolen_work.empty()) {
			shweeper->work_received_from_steal = std::move(_worksteal_requests[localId].stolen_work);
			LOG(V2_INFO, "%i variables sent from rank(%i) to rank(%i)id(%i) \n", shweeper->work_received_from_steal.size(), targetRank, _my_rank, localId);
			break;
		}
		//Unsuccessful global steal, try again
	}
	//Found work (or terminated), Tell the kissat/C thread where it can find the work
	*work = reinterpret_cast<unsigned int*>(shweeper->work_received_from_steal.data());
	*work_size = shweeper->work_received_from_steal.size();
	_shweepers_idle_count--;
}


void SweepJob::tryBeginBroadcastPing() {
	if (!_bcast) return;
	// Broadcast a message to all workers in your (sub) tree
	JobMessage msg = getMessageTemplate();
	msg.tag = _bcast->getMessageTag();
	msg.payload = {};
	_bcast->broadcast(std::move(msg));
}

void SweepJob::callback_for_broadcast_ping() {
	assert(_bcast);
	assert(_bcast->hasResult());

	auto snapshot = _bcast->getJobTreeSnapshot();
	// _bcast.reset();

	_bcast.reset(new JobTreeBroadcast(getId(), getJobTree().getSnapshot(), [this]() {callback_for_broadcast_ping();}, BCAST_INIT));

	JobMessage baseMsg = getMessageTemplate();
	baseMsg.tag = ALLRED;
	_red.reset(new JobTreeAllReduction(snapshot, baseMsg, std::vector<int>(), aggregateContributions));
	_red->contribute(std::move(_shweeper->eqs_to_share));
}



std::vector<int> SweepJob::aggregateContributions(std::list<std::vector<int>> &contribs) {
	//Each contribution has the format [Equivalences,Units, eq_size, unit_size].

	size_t total_size = NUM_SHARING_METADATA;
    for (const auto& vec : contribs) {
	    total_size += vec.size()-NUM_SHARING_METADATA;
    }
    std::vector<int> aggregated;
    aggregated.reserve(total_size);
	//Fill equivalences
	size_t total_eq_size = 0;
    for (const auto& contrib : contribs) {
    	int eq_size = contrib[contrib.size()-EQUIVS_SIZE_POS];
    	total_eq_size += eq_size;
		LOG(V3_VERB, "ß Element: %i eq_size \n", eq_size);
        aggregated.insert(aggregated.end(), contrib.begin(), contrib.begin()+eq_size);
    }
	//Fill units
	size_t total_unit_size = 0;
    for (const auto& contrib : contribs) {
    	int eq_size = contrib[contrib.size()-EQUIVS_SIZE_POS];
    	int unit_size = contrib[contrib.size()-UNITS_SIZE_POS];
		total_unit_size += unit_size;
		LOG(V3_VERB, "ß Element: %i unit_size \n", unit_size);
        aggregated.insert(aggregated.end(), contrib.begin()+eq_size, contrib.end()-NUM_SHARING_METADATA); //not copying the two counters at the end
    }
	bool all_idle = true;
    for (const auto& contrib : contribs) {
		bool idle = contrib[contrib.size()-IDLE_STATUS_POS];
    	all_idle &= idle;
		LOG(V3_VERB, "ß Element: idle == %i \n", idle);
    }
	aggregated.push_back(total_eq_size);
	aggregated.push_back(total_unit_size);
	aggregated.push_back(all_idle);
	LOG(V3_VERB, "ß Aggregated %i eqivalences, %i units, all_idle==%i \n", total_eq_size, total_unit_size, all_idle);
	assert(total_size == total_eq_size + total_unit_size + NUM_SHARING_METADATA);
    return aggregated;
}



std::vector<int> SweepJob::stealWorkFromAnyLocalSolver() {
	if (_shweepers_idle_count == _params.numThreadsPerProcess.val)
		return {}; //Dont need to cycle through local solvers if I know that all are also searching for work
    auto permutations = AdjustablePermutation::getPermutations(3, 10);
	for (auto vec : permutations) {
		for (int i : vec) {
			std::cout << i << " ";
		}
		std::cout << std::endl;
	}

	return {};
}

std::vector<int> SweepJob::stealWorkFromSpecificLocalSolver(int localId) {
	if (_terminate_all)
		return {};
	//We dont know how much there is to steal, so we ask
	KissatPtr shweeper = _shweepers[localId];
	size_t max_steal_amount = shweep_get_max_steal_amount(shweeper->solver);
	if (max_steal_amount == 0)
		return {};
	//There is potentially something to steal
	//Allocate memory for the steal here in C++, and pass it to kissat such that it can fill in the stolen work
	std::vector<int> stolen_work = std::vector<int>(max_steal_amount);
	int actually_stolen = shweep_steal_from_this_solver(shweeper->solver, reinterpret_cast<unsigned int*>(stolen_work.data()), max_steal_amount);
	if (actually_stolen == 0)
		return {};
	//We oversized the provided array a bit, such that C has in any case enough space to write
	//But since we only learn within C how much work there is actually to steal, we now at the C++ level reflect this information
	stolen_work.resize(actually_stolen);
	return stolen_work;
}


void SweepJob::loadFormula(KissatPtr shweeper) {
	const int* lits = getDescription().getFormulaPayload(0);
	const int payload_size = getDescription().getFormulaPayloadSize(0);
	for (int i = 0; i < payload_size ; i++) {
		shweeper->addLiteral(lits[i]);
	}
}















