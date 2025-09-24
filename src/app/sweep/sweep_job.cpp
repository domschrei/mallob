
#include "sweep_job.hpp"

#include "app/job.hpp"
#include "app/job_tree.hpp"
#include "util/logger.hpp"


SweepJob::SweepJob(const Parameters& params, const JobSetup& setup, AppMessageTable& table)
    : Job(params, setup, table)
{
        assert(_params.jobCommUpdatePeriod() > 0 || log_return_false("[ERROR] For this application to work,"
            " you must explicitly enable job communicators with the -jcup option, e.g., -jcup=0.1\n"));
	_worksteal_requests.resize(params.numThreadsPerProcess.val);
	for (int id = 0; id < params.numThreadsPerProcess.val; id++) {
		_worksteal_requests[id].searcher_id = id;
		_worksteal_requests[id].sent = true; //the initialized objects should not trigger a send
	}
	LOG(V2_INFO, "New SweepJob MPI Process with %i threads\n", params.numThreadsPerProcess.val);
}


void search_work_in_tree(void *SweepJob_state, unsigned **work, int *work_size) {
	// LOG(V2_INFO, "Shweep %i search_work_in_tree \n", ((SweepJob*) SweepJob_state)->_my_rank);
    ((SweepJob*) SweepJob_state)->searchWorkInTree(work, work_size);
}



void SweepJob::appl_start() {
	_my_rank = getJobTree().getRank();
	_my_index = getJobTree().getIndex();
	_is_root = getJobTree().isRoot();
	printf("ß Appl_start(): Rank %i, Index %i, is root? %i, Parent-Index %i, \n",   _my_rank, _my_index, _is_root, getJobTree().getParentIndex());
	printf("ß			  : num children %i\n", getJobTree().getNumChildren());
    _metadata = getSerializedDescription(0)->data();

	const JobDescription& desc = getDescription();

	for (int i=0; i < _params.numThreadsPerProcess.val; i++) {
		_shweepers.push_back(get_new_shweeper());
	}


	//Initialize _red already here, to make sure that all processes have a valid reduction object
	//maybe switch back to more robust "standard" sharing later
	JobMessage baseMsg = getMessageTemplate();
	baseMsg.tag = ALLRED;
	_red.reset(new JobTreeAllReduction(getJobTree().getSnapshot(), baseMsg, std::vector<int>(), aggregateContributions));
	_red->setCareAboutParent();

	for (auto shweeper : _shweepers) {
		std::future<void> fut_shweeper = ProcessWideThreadPool::get().addTask([&]() {
			LOG(V2_INFO, "Process loading formula  %i \n", _my_index);
			loadFormulaToShweeper();
			LOG(V2_INFO, "Process starting Shweeper %i \n", _my_index);
			LOG(V3_VERB, "Tree object is present? _red=%i \n", _red!=nullptr);
			int res = shweeper->solve(0, nullptr);
			LOG(V2_INFO, "\n # \n # \n Process finished Shweeper %i, result %i \n # \n # \n", _my_index, res);
			_internal_result.id = getId();
			_internal_result.revision = getRevision();
			_internal_result.result=res;
			_solved_status = 10;
			auto dummy_solution = std::vector<int>(1,0);
			_internal_result.setSolutionToSerialize((int*)(dummy_solution.data()), dummy_solution.size());
			_shweepers_running_count--;
		});
		_fut_shweepers.push_back( fut_shweeper);
		_shweepers_running_count++;
	}

	LOG(V3_VERB, "ß Finished SweepJob::appl_start()\n");

	// _red.reset(); This line will immediately kill _red for the new thread! Don't do this with my custom JobTreeAllReduction!

}

std::shared_ptr<Kissat> SweepJob::get_new_shweeper() {
	SolverSetup setup;
	setup.logger = &Logger::getMainInstance();
	setup.jobname = "swissat-"+to_string(_my_index);
	setup.numVars = desc.getAppConfiguration().fixedSizeEntryToInt("__NV");
	setup.numOriginalClauses = desc.getAppConfiguration().fixedSizeEntryToInt("__NC");
	// printf("ß [%i] Payload: %i vars, %i clauses \n", _my_index, setup.numVars, setup.numOriginalClauses);

	std::shared_ptr<Kissat> shweeper(new Kissat(setup));
	shweeper->set_option("mallob_custom_sweep_verbosity", 2); //0: No custom kissat messages. 1: Some. 2: More
	shweeper->set_option("mallob_solver_count", NUM_WORKERS);
	shweeper->set_option("mallob_solver_id", _my_index);
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
	shweeper->set_option("sweepcomplete", 1); //deactivates any tick limits on sweeping
	shweeper->set_option("probe", 1); //there is some cleanup-probing at the end of the sweeping
	return shweeper;
}

// Called periodically by the main thread to allow the worker to emit messages.
void SweepJob::appl_communicate() {

	// LOG(V3_VERB, "ß appl_communicate \n");
	double elapsed_time = Timer::elapsedSeconds();
	double wait_time = 0.001;
	bool can_start = elapsed_time > wait_time;


	//Worksteal requests need to be execute by the main MPI thread, because sending MPI messages via a callback from C can cause undefined behaviour
	for (auto request : _worksteal_requests) {
		if (!request.sent) {
			request.sent = true;
			JobMessage msg = getMessageTemplate();
			msg.tag = TAG_SEARCHING_WORK;
			//Need to add these two fields because we are doing arbitrary point-to-point communication
			msg.treeIndexOfDestination = request.target_rank;
			msg.contextIdOfDestination = getJobComm().getContextIdOrZero(request.target_rank);
			msg.payload = {request.searcher_id};
			// LOG(V2_INFO, "Rank %i asks rank %i for work\n", _my_rank, recv_rank, n);
			// LOG(V2_INFO, "  with destionation ctx_id %i \n", msg.contextIdOfDestination);
			getJobTree().send(request.target_rank, MSG_SEND_APPLICATION_MESSAGE, msg);
		}
	}


	if (can_start && getVolume() == NUM_WORKERS && getJobComm().getWorldRankOrMinusOne(NUM_WORKERS-1) >= 0) {
		// LOG(V3_VERB, "ß appl_communicate. _red=%i \n", _red!=nullptr);
		// LOG(V3_VERB, "ß have %i eqs to share\n", _shweeper->eqs_to_share.size());
		// LOG(V3_VERB, "ß have %i units to share\n", _shweeper->units_to_share.size());
		bool reset_red = false;
		if (_red && _red->hasResult()) {
			//store the received equivalences such that the local solver than eventually import them
			auto share_received = _red->extractResult();
			const int received_eq_size = share_received[share_received.size()-EQS_SIZE_POS];
			const int received_unit_size = share_received[share_received.size()-UNITS_SIZE_POS];
			const int all_idle = share_received[share_received.size()-IDLE_STATUS_POS];
			LOG(V1_WARN, "ß --- Received Broadcast: %i eq_size, %i unit_size -- \n", received_eq_size, received_unit_size);
			if (all_idle) {
				_terminate = true;
				LOG(V1_WARN, "ß # \n # \n  --- ALL SWEEPERS IDLE - CAN TERMINATE -- \n # \n # \n");
			}

			//save equivalences
			auto& eqs_received = _shweeper->eqs_received_from_sharing;
			eqs_received.reserve(eqs_received.size() + received_eq_size);
			eqs_received.insert(
				eqs_received.end(),
				std::make_move_iterator(share_received.begin()),
				std::make_move_iterator(share_received.begin() + received_eq_size)
			);
			//save units
			auto& units_received = _shweeper->units_received_from_sharing;
			units_received.reserve(units_received.size() + received_unit_size);
			units_received.insert(
				units_received.end(),
				std::make_move_iterator(share_received.begin() + received_eq_size),
				std::make_move_iterator(share_received.end()   - NUM_SHARING_METADATA)
			);
			reset_red = true;
			// parent_is_ready = _red->isParentReady();
			LOG(V3_VERB, "ß Now storing %i equivalences and %i units, for local solver to import \n", eqs_received.size()/2, units_received.size());
		}

		if (!started_sharing) { //triggers the very first construction
			reset_red = true;
			// parent_is_ready = true;
			started_sharing = true;
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
		_shweeper->work_stolen_from_local_solver = {};
		int actually_stolen = steal_from_my_local_solver();
		if (actually_stolen>0) {
			//the steal vector was a bit over-dimensioned before the steal, because we only knew an upper limit, but learned the actual amount only  while stealing
			//to elegantly communicate this actual steal amount via MPI, we resize the vector to contain this information in its .size()
			_shweeper->work_stolen_from_local_solver.resize(actually_stolen);
			msg.tag = TAG_SUCCESSFUL_WORK_STEAL;
			// LOG(V2_INFO, "Shweep rank %i: providing %i work, sending to source %i \n",  _my_rank, stolen, source);
		} else {
			// LOG(V2_INFO, "Shweep rank %i: didn't have work to give to source %i\n", _my_rank, source);
			msg.tag = TAG_UNSUCCESSFUL_WORK_STEAL;
		}
		//send back to source
		int searcher_id = msg.payload[0];
		_shweeper->work_stolen_from_local_solver.push_back(searcher_id);
		msg.payload = std::move(_shweeper->work_stolen_from_local_solver);
		msg.treeIndexOfDestination = source;
		msg.contextIdOfDestination = getJobComm().getContextIdOrZero(source);
		getJobTree().send(source, MSG_SEND_APPLICATION_MESSAGE, msg);
	}
	else if (msg.tag == TAG_SUCCESSFUL_WORK_STEAL) {
		int searcher_id = msg.payload.back();
		msg.payload.pop_back();
		_worksteal_requests[searcher_id].stolen_work = std::move(msg.payload);
		_worksteal_requests[searcher_id].got_steal_response = true;
	}
	else if (msg.tag == TAG_UNSUCCESSFUL_WORK_STEAL) {
		int searcher_id = msg.payload.back();
		msg.payload.pop_back();
		_worksteal_requests[searcher_id].got_steal_response = true;
	}
}


void SweepJob::searchWorkInTree(unsigned **work, int *work_size) {
	_shweeper->shweep_is_idle = true;
	_shweeper->work_received_from_others = {};

	if (_my_rank == 0 && !root_received_work) {
	    //to know how much space we need to allocate for all variables, we assume that the maximum index of any variable corresponds to the total number of variables -1,
	    //i.e. that there are no holes in the numbering. This is an assumption that standard Kissat makes all the time, so we also do it here
		unsigned VARS = shweep_get_num_vars(_shweeper->solver);
		_shweeper->work_received_from_others = std::vector<int>(VARS);
		//the initial work: all variables
		for (int idx = 0; idx < VARS; idx++) {
			_shweeper->work_received_from_others[idx] = idx;
		}
		*work = reinterpret_cast<unsigned int*>(_shweeper->work_received_from_others.data());
		*work_size = VARS;
		root_received_work = true;
		_shweeper->shweep_is_idle = false;
		LOG(V2_INFO, "Shweep root %i requested work, got all %u variables\n", _my_rank, VARS);
		return;
	}

	SplitMix64Rng _rng;
	while (_shweeper->work_received_from_others.empty()) {
		if (_terminate) {
			*work = reinterpret_cast<unsigned int*>(_shweeper->work_received_from_others.data());
			*work_size = 0; //this zero tells the kissat solver that we are finished
			break;
		}
		int n = getVolume();
		//Steal from a random other solver
		int recv_index = _rng.randomInRange(0,n);
        int recv_rank = getJobComm().getWorldRankOrMinusOne(recv_index);

		// LOG(V2_INFO, "random index: %i  (volume %i)\n", recv_index, n);
        if (recv_rank == -1) {
			// tree not yet fully built, LOG(V2_INFO, "No receive rank found for index %i, try new random index !\n", recv_index);
			usleep(100);
        	continue;
        }

		if (recv_rank == _my_rank) {//not asking ourselves for work
			continue;
		}

		int searcher_id = 0; //after hybridization, the kissat solver must provide this information
		WorkstealRequest request;
		request.searcher_id = searcher_id;
		request.target_rank = recv_rank;
		_worksteal_requests[searcher_id] = request;

		while(_worksteal_requests[searcher_id].got_steal_response == false) {
			usleep(100);
		}


		if (!_worksteal_requests[searcher_id].stolen_work.empty()) {
			_shweeper->shweep_is_idle = false;
			//Tell C/Kissat where it can read the new work
			*work = reinterpret_cast<unsigned int*>(_shweeper->work_received_from_others.data());
			*work_size = _shweeper->work_received_from_others.size();
			LOG(V2_INFO, "%i variables sent from rank(%i) to rank(%i) \n", _shweeper->work_received_from_others.size(), _my_rank, recv_rank);
			break;
		}
		// LOG(V2_INFO, "Rank %i did not received work from rank %i\n", _my_rank, recv_rank);
	}
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
    	int eq_size = contrib[contrib.size()-EQS_SIZE_POS];
    	total_eq_size += eq_size;
		LOG(V3_VERB, "ß Element: %i eq_size \n", eq_size);
        aggregated.insert(aggregated.end(), contrib.begin(), contrib.begin()+eq_size);
    }
	//Fill units
	size_t total_unit_size = 0;
    for (const auto& contrib : contribs) {
    	int eq_size = contrib[contrib.size()-EQS_SIZE_POS];
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


void SweepJob::advanceSweepMessage(JobMessage& msg) {

}

int SweepJob::steal_from_my_local_solver() {
	if (_terminate)
		return 0;
	//We dont know how much there is to steal, so we ask
	size_t max_steal_amount = shweep_get_max_steal_amount(_shweeper->solver);
	if (max_steal_amount == 0)
		return 0;
	//There is potentially something to steal, allocate memory for it, and pass it to kissat for filling
	_shweeper->work_stolen_from_local_solver.resize(max_steal_amount);
	int actually_stolen = shweep_steal_from_this_solver(_shweeper->solver, reinterpret_cast<unsigned int*>(_shweeper->work_stolen_from_local_solver.data()), max_steal_amount);
	return actually_stolen;
}


void SweepJob::loadFormulaToShweeper() {
	const int* lits = getDescription().getFormulaPayload(0);
	const int payload_size = getDescription().getFormulaPayloadSize(0);
	for (int i = 0; i < payload_size ; i++) {
		_shweeper->addLiteral(lits[i]);
	}
}















