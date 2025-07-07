
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
	_swissat->activateLearnedEquivalenceCallbacks();

    // Basic configuration options for all solvers
    _swissat->set_option("quiet", 1); // suppress any standard kissat output
    _swissat->set_option("verbose", 0); // set the native kissat verbosity
    _swissat->set_option("check", 0); // do not check model or derived clauses
    _swissat->set_option("profile",3); // do detailed profiling how much time we spent where
	_swissat->set_option("seed", 0);   // always start with the same seed

	_swissat->set_option("sweep", 1); //We obviously want sweeping
	_swissat->set_option("simplify", 1); //Activating simplify extremely boosts the frequency that sweep is scheduled, take it for now

    _swissat->set_option("factor", 0); //No bounded variable addition
	_swissat->set_option("eliminate", 0); //No Bounded Variable Elimination
	_swissat->set_option("substitute", 0); //No equivalent literal substitution (both obstruct import)

	_swissat->set_option("fastel", 0); //No fast elimination. Fast elimination sets import->eliminated = true, which interrupts further equivalence imports
	//Technically, we could instead maybe also follow through with the eternal-to-internal literal import, and ignore the eliminated-flag
	//since we then anyways translate that intermediate literal again via sweeper->repr[..] to a (hopefully) valid literal
	//so temporarily having an eliminated literal might be ok if we only use it to index its valid representative literal...

	_swissat->set_option("lucky", 0);     //These operations do not obstruct sweep, but to keep everything simple we deactivate them for now
	_swissat->set_option("congruence", 0);
	_swissat->set_option("backbone", 0);
	_swissat->set_option("transitive", 0);
	_swissat->set_option("vivify", 0);

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

	if (getVolume() == NUM_WORKERS && getJobComm().getWorldRankOrMinusOne(NUM_WORKERS-1) >= 0) {


		// LOG(V3_VERB, "ß appl_communicate full volume \n");

		LOG(V3_VERB, "ß have %i \n", _swissat->stored_equivalences_to_share.size());
		bool reset_red = false;
		if (_red && _red->hasResult()) {
			//store the received equivalences such that the local solver than eventually import them
			auto share_received = _red->extractResult();
			LOG(V3_VERB, "ß --- Received Broadcast Result, Extracted Size %i --- \n", share_received.size());
			auto& local_store = _swissat->stored_equivalences_to_import;
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
			LOG(V3_VERB, "ß contributing %i\n", _swissat->stored_equivalences_to_share.size());
			_red->contribute(std::move(_swissat->stored_equivalences_to_share));
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
	// LOG(V3_VERB, "ß appl_communicate end \n");



// React to an incoming message. (This becomes relevant only if you send custom messages)
void SweepJob::appl_communicate(int source, int mpiTag, JobMessage& msg) {
#if SWEEP_COMM_TYPE == 1
	//locally store equivalences received from sharing, for kissat to import them at will
	auto &local_store = _swissat->stored_equivalences_to_import;
	local_store.reserve(local_store.size() + msg.payload.size());
	local_store.insert(local_store.end(), msg.payload.begin(), msg.payload.end());

	if (_my_index == 0) {
		LOG(V2_INFO, "[sweep] Rec %i  Chain ended, arrived back to root\n \n", msg.payload.size()/2);
	} else {
		advanceSweepMessage(msg);
	}
#endif
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
	_red->contribute(std::move(_swissat->stored_equivalences_to_share));
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


void SweepJob::loadFormulaToSwissat() {
	const int* lits = getDescription().getFormulaPayload(0);
	const int payload_size = getDescription().getFormulaPayloadSize(0);
	for (int i = 0; i < payload_size ; i++) {
		_swissat->addLiteral(lits[i]);
	}
}















