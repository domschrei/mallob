
#include "sweep_job.hpp"

#include "app/job.hpp"
#include "app/job_tree.hpp"
#include "util/logger.hpp"

#define SWEEP_COMM_TYPE 2


SweepJob::SweepJob(const Parameters& params, const JobSetup& setup, AppMessageTable& table)
    : Job(params, setup, table) {
        assert(_params.jobCommUpdatePeriod() > 0 || log_return_false("[ERROR] For this application to work,"
            " you must explicitly enable job communicators with the -jcup option, e.g., -jcup=0.1\n"));
}


void search_work_in_tree(void *SweepJob_state, int **work, int *work_size) {
    ((SweepJob*) SweepJob_state)->searchWorkInTree(work, work_size);
}



void SweepJob::appl_start() {
	_my_rank = getJobTree().getRank();
	_my_index = getJobTree().getIndex();
	_is_root = getJobTree().isRoot();
	printf("ß Appl_start(): is root? %i, Parent-Index %i, Rank %i, Index %i, \n",  _is_root, getJobTree().getParentIndex(), _my_rank, _my_index);
	printf("ß			  : num children %i", getJobTree().getNumChildren());
    _metadata = getSerializedDescription(0)->data();

	const JobDescription& desc = getDescription();

	SolverSetup setup;
	setup.logger = &Logger::getMainInstance();
	setup.jobname = "swissat-"+to_string(_my_index);
	setup.numVars = desc.getAppConfiguration().fixedSizeEntryToInt("__NV");
	setup.numOriginalClauses = desc.getAppConfiguration().fixedSizeEntryToInt("__NC");
	// printf("ß [%i] Payload: %i vars, %i clauses \n", _my_index, setup.numVars, setup.numOriginalClauses);

	_swissat.reset(new Kissat(setup));
	_swissat->set_option("mallob_custom_sweep_verbosity", 1); //0: No custom messages. 1: Some. 2: Verbose
	_swissat->set_option("mallob_solver_count", NUM_WORKERS);
	_swissat->set_option("mallob_solver_id", _my_index);
	_swissat->activateEqImportExportCallbacks();
	_swissat->shweep_SetSearchWorkCallback(this, &search_work_in_tree);

    // Basic configuration options for all solvers
    _swissat->set_option("quiet", 1); // suppress any standard kissat output
    _swissat->set_option("verbose", 0); // set the native kissat verbosity
    _swissat->set_option("check", 0); // do not check model or derived clauses
    _swissat->set_option("profile",3); // do detailed profiling how much time we spent where
	_swissat->set_option("seed", 0);   // always start with the same seed

	_swissat->set_option("mallob_shweep", 1); //Jumps directly to shared sweeping and bypasses everything else

	//Initialize _red already here, to make sure that all processes have a valid reduction object
	JobMessage baseMsg = getMessageTemplate();
	baseMsg.tag = ALLRED;
	_red.reset(new JobTreeAllReduction(getJobTree().getSnapshot(), baseMsg, std::vector<int>(), aggregateContributions));
	_red->careAboutParentStatus();

	_swissat_running_count++;
	_fut_swissat = ProcessWideThreadPool::get().addTask([&]() {
		LOG(V2_INFO, "Process loading formula  %i \n", _my_index);
		loadFormulaToSwissat();
		LOG(V2_INFO, "Process starting Swissat %i \n", _my_index);
		int res = _swissat->solve(0, nullptr);
		LOG(V2_INFO, "\n # \n # \n Process finished Swissat %i, result %i \n # \n # \n", _my_index, res);
		_internal_result.id = getId();
		_internal_result.revision = getRevision();
		_internal_result.result=res;
		auto dummy_solution = std::vector<int>(1,0);
		_internal_result.setSolutionToSerialize((int*)(dummy_solution.data()), dummy_solution.size());
		_swissat_running_count--;
	});

	LOG(V3_VERB, "ß finished appl_start\n");

}



// Called periodically by the main thread to allow the worker to emit messages.
void SweepJob::appl_communicate() {

	#if SWEEP_COMM_TYPE == 1
	// Not enough workers available?
	if (getJobTree().isRoot() && getVolume() < NUM_WORKERS) {
		if (getAgeSinceActivation() < 1) return; // wait for up to 1s after appl_start
		LOG(V2_INFO, "[sweep] Unable to get %i workers within 1 second - giving up\n", NUM_WORKERS);
		// Report an "unknown" result (code 0)
		// insertResult(0, {-1});
		// _started_roundtrip = true;
		return;
	}

	// Workers available and valid job communicator present?
	if (getJobTree().isRoot() && getVolume() == NUM_WORKERS && getJobComm().getWorldRankOrMinusOne(NUM_WORKERS-1) >= 0) {
		// craft a message to initiate sweep communication
		JobMessage msg = getMessageTemplate();
		msg.tag = MSG_SWEEP;
		msg.payload.clear();
		advanceSweepMessage(msg);
	}
	#elif SWEEP_COMM_TYPE == 2

	// auto list = getJobComm().getAddressList();
	// for (auto &l : list) {
	// 	LOG(V3_VERB, "list has %i \n", l.rank);
	// }
	LOG(V3_VERB, "ß appl_communicate \n");
	double elapsed_time = Timer::elapsedSeconds();
	double wait_time = 0.001;
	bool can_start = elapsed_time > wait_time;

	if (can_start && getVolume() == NUM_WORKERS && getJobComm().getWorldRankOrMinusOne(NUM_WORKERS-1) >= 0) {
		// LOG(V3_VERB, "ß appl_communicate full volume \n");

		LOG(V3_VERB, "ß have %i \n", _swissat->eqs_to_share.size());
		bool reset_red = false;
		if (_red && _red->hasResult()) {
			//store the received equivalences such that the local solver than eventually import them
			auto share_received = _red->extractResult();
			LOG(V3_VERB, "ß --- Received Broadcast Result, Extracted Size %i --- \n", share_received.size());
			auto& local_store = _swissat->eqs_to_pass_down;
			local_store.reserve(local_store.size() + share_received.size());
			local_store.insert(
				local_store.end(),
				std::make_move_iterator(share_received.begin()),
				std::make_move_iterator(share_received.end())
			);
			reset_red = true;
			// parent_is_ready = _red->isParentReady();
			LOG(V3_VERB, "ß Storing %i equivalences for local solver to import \n", local_store.size());
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
			_red->careAboutParentStatus();
			_red->tellChildrenParentIsReady();
			LOG(V3_VERB, "ß contributing %i\n", _swissat->eqs_to_share.size());
			_red->contribute(std::move(_swissat->eqs_to_share));
			if (parent_was_ready) {
				_red->enableParentIsReady();
			}
		}
		_red->advance();
	}
	#elif SWEEP_COMM_TYPE == 3


	if (getVolume() == NUM_WORKERS && getJobComm().getWorldRankOrMinusOne(NUM_WORKERS-1) >= 0) {
		if (_is_root) {
			if (!_red || _red->finishedAndNoLongerValid()) {
				tryBeginBroadcastPing();
			}
		}
		tryExtractResult();
	}
	#endif
}



// React to an incoming message. (This becomes relevant only if you send custom messages)
void SweepJob::appl_communicate(int source, int mpiTag, JobMessage& msg) {
	LOG(V2_INFO, "Shweep rank %i: received custom message from source %i, mpiTag %i, msg.tag %i \n", _my_rank, source, mpiTag, msg.tag);
	if (msg.tag == TAG_SEARCHING_WORK) {
		if (steal_from_my_local_solver()>0) {
			msg.payload = std::move(_swissat->work_stolen_locally);
			msg.tag = TAG_SUCCESSFUL_WORK_STEAL;
			LOG(V2_INFO, "Shweep rank %i: sending own work back to source %i \n", _my_rank, source);
		} else {
			LOG(V2_INFO, "Shweep rank %i: didn't have work to give to source %i\n", _my_rank, source);
			msg.tag = TAG_UNSUCCESSFUL_WORK_STEAL;
		}
		getJobTree().send(source, MSG_SEND_APPLICATION_MESSAGE, msg);
	}
	if (msg.tag == TAG_SUCCESSFUL_WORK_STEAL) {
		LOG(V2_INFO, "Shweep rank %i: received work from source %i\n", _my_rank, source);
		_swissat->my_work = std::move(msg.payload);
		got_steal_response = true;
	}
	if (msg.tag == TAG_UNSUCCESSFUL_WORK_STEAL) {
		LOG(V2_INFO, "Shweep rank %i: didnt receive any work from source %i\n", _my_rank, source);
		got_steal_response = true;
	}
}


void SweepJob::searchWorkInTree(unsigned **work, unsigned *work_size) {
	shweep_state = SHWEEP_STATE_IDLE;
	_swissat->my_work = {};

	if (_my_rank == 0 && !formula_initially_provided) {

	}

	while (_swissat->my_work.empty()) {
		int n = getVolume();
		SplitMix64Rng _rng;
		int rank = _rng.randomInRange(0,n);
		got_steal_response = false;

		JobMessage msg = getMessageTemplate();
		msg.tag = TAG_SEARCHING_WORK;

		getJobTree().send(rank, MSG_SEND_APPLICATION_MESSAGE, msg);
		LOG(V2_INFO, "Rank %u asks rank %u for work\n", _my_index, rank);

		while (!got_steal_response) {
			usleep(100 /*0.1 millisecond*/);
		}
		if (!_swissat->my_work.empty()) {
			shweep_state = SHWEEP_STATE_WORKING;
			//Tell C/Kissat where it can read the new work
			*work = _swissat->my_work.data();
			*work_size = _swissat->my_work.size();
			LOG(V2_INFO, "Rank %u asks rank %u for work: successfully stole %u work \n", _my_index, rank, _swissat->my_work.size());
			break;
		}
		LOG(V2_INFO, "Rank %u asks rank %u for work: failed, no work either\n", _my_index, rank);
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
	_red->contribute(std::move(_swissat->eqs_to_share));
}


// void SweepJob::tryExtractResult() {
// 	if (!_red) return;
// 	_red->advance();
// 	if (!_red.hasResult()) return;
//
// 	auto received_reduction = _red->extractResult();
// 	LOG(V3_VERB, "ß --- Received global Reduction Result, Extracted Size %i --- \n", received_reduction.size());
// 	auto& local_store = _swissat->stored_equivalences_to_import;
// 	local_store.reserve(local_store.size() + received_reduction.size());
// 	local_store.insert(
// 		local_store.end(),
// 		std::make_move_iterator(received_reduction.begin()),
// 		std::make_move_iterator(received_reduction.end())
// 	);
//
// 	LOG(V3_VERB, "ß Storing %i equivalences for local solver to import \n", local_store.size());
// 	// _red.reset();
// 	//here is the logical point to reset _red and contribute again
// 	//use broadcast only to communicate parent_is_ready
// 	//but: parent_is_ready can't just be forwarded also by the children, because they themselves might not yet be ready...
// }

std::vector<int> SweepJob::aggregateContributions(std::list<std::vector<int>> &contribs) {
    size_t totalSize = 0;
    for (const auto& vec : contribs) totalSize += vec.size();
    std::vector<int> aggregated;
    aggregated.reserve(totalSize);
    for (const auto& contrib : contribs) {
        aggregated.insert(aggregated.end(), contrib.begin(), contrib.end());
    }
	LOG(V3_VERB, "ß Aggregated %i \n", totalSize);
    return aggregated;
}


void SweepJob::advanceSweepMessage(JobMessage& msg) {
#if SWEEP_COMM_TYPE == 1
	int incoming_eqs = msg.payload.size()/2;
	//Transfer local data to the message
	auto &local_eqs = _swissat->stored_equivalences_to_share;
	msg.payload.reserve(msg.payload.size() + local_eqs.size());
	msg.payload.insert(msg.payload.end(), local_eqs.begin(), local_eqs.end());

	//Now we can delete the equivalences locally, to make place for new ones
	int added_eqs = _swissat->stored_equivalences_to_share.size()/2;
	_swissat->stored_equivalences_to_share.clear();

	//Sending data in a circle, in order of the indices
	int receiver_index = (_my_index + 1) % NUM_WORKERS;
	int receiver_rank = getJobComm().getWorldRankOrMinusOne(receiver_index);
	msg.treeIndexOfDestination = receiver_index;
	msg.contextIdOfDestination = getJobComm().getContextIdOrZero(receiver_index);
	assert(msg.contextIdOfDestination != 0);

	LOG(V2_INFO, "[sweep] Rec %i   Add %i    Send %i  (to rank %i) \n", incoming_eqs, added_eqs, msg.payload.size()/2, receiver_rank);
	getJobTree().send(receiver_rank, MSG_SEND_APPLICATION_MESSAGE, msg);
#endif
}

int SweepJob::steal_from_my_local_solver() {
	//We dont know how much there is to steal, so we ask
	size_t steal_amount = shweep_get_steal_amount(_swissat->solver);
	if (steal_amount == 0)
		return 0;
	//There is something to steal, allocate memory for it, and pass it to kissat for filling
	_swissat->work_stolen_locally.resize(steal_amount);
	shweep_steal_from_this_solver(_swissat->solver, _swissat->work_stolen_locally.data(), steal_amount);
	return steal_amount;
}



void SweepJob::loadFormulaToSwissat() {
	const int* lits = getDescription().getFormulaPayload(0);
	const int payload_size = getDescription().getFormulaPayloadSize(0);
	for (int i = 0; i < payload_size ; i++) {
		_swissat->addLiteral(lits[i]);
	}
}















