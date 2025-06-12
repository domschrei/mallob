
#include "sweep_job.hpp"


#include "app/job.hpp"
#include "app/job_tree.hpp"
#include "comm/job_tree_all_reduction.hpp"
#include "util/logger.hpp"


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
    _metadata = getSerializedDescription(0)->data();

	const JobDescription& desc = getDescription();

	SolverSetup setup;
	setup.logger = &Logger::getMainInstance();
	setup.jobname = "swissat-"+to_string(_my_index);
	setup.numVars = desc.getAppConfiguration().fixedSizeEntryToInt("__NV");
	setup.numOriginalClauses = desc.getAppConfiguration().fixedSizeEntryToInt("__NC");
	// printf("ß [%i] Payload: %i vars, %i clauses \n", _my_index, setup.numVars, setup.numOriginalClauses);

	_swissat.reset(new Kissat(setup));
	_swissat->set_option_externally("mallob_solver_id", _my_index);
	_swissat->set_option_externally("mallob_solver_count", NUM_WORKERS);
	_swissat->set_option_externally("mallob_custom_sweep_verbosity", 0); //0: No messages. 1: Verbose Messaged.
	_swissat->set_option_externally("quiet", 1);
	_swissat->activateLearnedEquivalenceCallback();


	_swissat_running_count++;
	_fut_swissat = ProcessWideThreadPool::get().addTask([&]() {
		LOG(V2_INFO, "Process loading formula  %i \n", _my_index);
		loadFormulaToSwissat();
		LOG(V2_INFO, "Process starting Swissat %i \n", _my_index);
		int res = _swissat->solve(0, nullptr);
		LOG(V2_INFO, "Process finished Swissat %i, result %i\n", _my_index, res);
		_internal_result.id = getId();
		_internal_result.revision = getRevision();
		_internal_result.result=res;
		auto dummy_solution = std::vector<int>(1,0);
		_internal_result.setSolutionToSerialize((int*)(dummy_solution.data()), dummy_solution.size());
		_swissat_running_count--;
	});



	// sleep(20);
	// printf("ß [%i] Swissat result written internally\n", _my_index);
	// printf("ß [%i] Swissat stopped end sleep \n", _my_index);
	// _solved_status = 1;
}



// Called periodically by the main thread to allow the worker to emit messages.
void SweepJob::appl_communicate() {
	// Not enough workers available?
	if (getJobTree().isRoot() && getVolume() < NUM_WORKERS) {
		if (getAgeSinceActivation() < 1) return; // wait for up to 1s after appl_start
		LOG(V2_INFO, "[sweep] Unable to get %i workers within 1 second - giving up\n", NUM_WORKERS);
		// Report an "unknown" result (code 0)
		// insertResult(0, {-1});
		// _started_roundtrip = true;
		return;
	}

	// LOG(V2_INFO, "[sweep] Equivalences to share: %i \n", _swissat->stored_equivalences.size()/2);

	// Workers available and valid job communicator present?
	if (getJobTree().isRoot() && getVolume() == NUM_WORKERS && getJobComm().getWorldRankOrMinusOne(NUM_WORKERS-1) >= 0) {
		// craft a message to initiate sweep communication
		JobMessage msg = getMessageTemplate();
		msg.tag = MSG_SWEEP;
		msg.payload.clear();
		advanceSweepMessage(msg);
	}
}


// React to an incoming message. (This becomes relevant only if you send custom messages)
void SweepJob::appl_communicate(int source, int mpiTag, JobMessage& msg) {
	LOG(V2_INFO, "[sweep] Got %i Equivalences   (%i,%i)...(%i,%i)  \n", msg.payload.size()/2, msg.payload[0], msg.payload[1], msg.payload.end()[-2], msg.payload.end()[-1]);
	if (_my_index == 0) {
		LOG(V2_INFO, "[sweep] Chain ended, arrived back to root\n \n");
	} else {
		advanceSweepMessage(msg);
	}
}


void SweepJob::advanceSweepMessage(JobMessage& msg) {

	//Transfer local data to the message
	LOG(V2_INFO, "[sweep] Add: %i \n", _swissat->stored_equivalences.size()/2);
	auto &local_eqs = _swissat->stored_equivalences;
	msg.payload.reserve(msg.payload.size() + local_eqs.size());
	msg.payload.insert(msg.payload.end(), local_eqs.begin(), local_eqs.end());

	//Now we can delete the equivalences locally, to make place for new ones
	_swissat->stored_equivalences.clear();

	//Sending data in a circle, in order of the indices
	int receiver_index = (_my_index + 1) % NUM_WORKERS;
	int receiver_rank = getJobComm().getWorldRankOrMinusOne(receiver_index);
	msg.treeIndexOfDestination = receiver_index;
	msg.contextIdOfDestination = getJobComm().getContextIdOrZero(receiver_index);
	assert(msg.contextIdOfDestination != 0);
	getJobTree().send(receiver_rank, MSG_SEND_APPLICATION_MESSAGE, msg);
}


void SweepJob::loadFormulaToSwissat() {
	const int* lits = getDescription().getFormulaPayload(0);
	const int payload_size = getDescription().getFormulaPayloadSize(0);
	for (int i = 0; i < payload_size ; i++) {
		_swissat->addLiteral(lits[i]);
	}
}















