
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
	printf("ß appl_start\n");
	_my_rank = getJobTree().getRank();
	_my_index = getJobTree().getIndex();
	_is_root = getJobTree().isRoot();
	printf("ß Rank %i, Index %i, is root? %i, Parent-Index %i\n", _my_rank, _my_index, _is_root, getJobTree().getParentIndex());
    _metadata = getSerializedDescription(0)->data();

	const JobDescription& desc = getDescription();

	SolverSetup setup;
	setup.logger = &Logger::getMainInstance();
	setup.jobname = "swissat-"+to_string(_my_index);
	setup.numVars = desc.getAppConfiguration().fixedSizeEntryToInt("__NV");
	setup.numOriginalClauses = desc.getAppConfiguration().fixedSizeEntryToInt("__NC");
	printf("ß [%i] Payload: %i vars, %i clauses \n", _my_index, setup.numVars, setup.numOriginalClauses);

	_swissat.reset(new Kissat(setup));
	_swissat->set_option_externally("mallob_solver_id", _my_index);
	_swissat->set_option_externally("mallob_solver_count", NUM_WORKERS);
	_swissat->set_option_externally("mallob_custom_verbosity", 0);
	_swissat->set_option_externally("quiet", 1);

	const int* lits = getDescription().getFormulaPayload(0);
	const int payload_size = getDescription().getFormulaPayloadSize(0);
	for (int i = 0; i < payload_size ; i++) {
		_swissat->addLiteral(lits[i]);
	}
	int res = _swissat->solve(0, nullptr);
	printf("ß [%i] Swissat result: %i\n", _my_index, res);

	// sleep(20);
	// printf("ß [%i] Swissat result written internally\n", _my_index);
	// printf("ß [%i] Swissat stopped end sleep \n", _my_index);
	// _solved_status = 1;
	_internal_result.id = getId();
    _internal_result.revision = getRevision();
    _internal_result.result=res;
	auto dummy_solution = std::vector<int>(1,0);
	_internal_result.setSolutionToSerialize((int*)(dummy_solution.data()), dummy_solution.size());
}



// Called periodically by the main thread to allow the worker to emit messages.
void SweepJob::appl_communicate() {
	// Not enough workers available?

	// printf("ß appl_communicate() %i \n", _my_index);

	if (getJobTree().isRoot() && getVolume() < NUM_WORKERS) {
		if (getAgeSinceActivation() < 1) return; // wait for up to 1s after appl_start
		LOG(V2_INFO, "[sweep] Unable to get %i workers within 1 second - giving up\n", NUM_WORKERS);
		// Report an "unknown" result (code 0)
		// insertResult(0, {-1});
		// _started_roundtrip = true;
		return;
	}

	// printf("ß appl_communicate() %i ready for starting (isRoot=%i), Volume=%i, Children %i \n",
		// _my_index, getJobTree().isRoot(), getVolume(), getJobTree().getNumChildren());
	// printf("ß rank(0) = %i \n", getJobComm().getWorldRankOrMinusOne(0));
	// printf("ß rank(1) = %i \n", getJobComm().getWorldRankOrMinusOne(1));
	// printf("ß rank(2) = %i \n", getJobComm().getWorldRankOrMinusOne(2));
	// printf("ß rank(3) = %i \n", getJobComm().getWorldRankOrMinusOne(3));


	// Workers available and valid job communicator present?
	if (getJobTree().isRoot() && getVolume() == NUM_WORKERS && getJobComm().getWorldRankOrMinusOne(NUM_WORKERS-1) >= 0) {
		// craft a message to initiate sweep communication
		JobMessage msg = getMessageTemplate();
		msg.tag = MSG_SWEEP;
		msg.payload = {0}; // indicates the number of bounces so far
		// LOG(V2_INFO, "[sweep-%i] starting chain\n", _my_index);
		advanceSweepMessage(msg);
	}
}


// React to an incoming message. (This becomes relevant only if you send custom messages)
void SweepJob::appl_communicate(int source, int mpiTag, JobMessage& msg) {
	int bounces = msg.payload[0];
	// LOG(V2_INFO, "[sweep-%i] read msg: %i bounces\n", _my_index, bounces);
	if (bounces == NUM_WORKERS-1) {
		LOG(V2_INFO, "[sweep-%i] chain stopped\n", _my_index);
		// insertResult(10, std::vector<int>(msg.payload.begin()+1, msg.payload.end()));
	} else {
		advanceSweepMessage(msg);
	}
}


void SweepJob::advanceSweepMessage(JobMessage& msg) {
	int bounces = msg.payload[0];
	int receiver_index = bounces + 1;
	int receiver_rank = getJobComm().getWorldRankOrMinusOne(receiver_index);
	LOG(V2_INFO, "[sweep-%i] send to (%i, %i)\n", _my_index, receiver_index, receiver_rank);
	msg.payload[0]++;
	msg.treeIndexOfDestination = receiver_index;
	msg.contextIdOfDestination = getJobComm().getContextIdOrZero(receiver_index);
	assert(msg.contextIdOfDestination != 0);
	// Send
	getJobTree().send(receiver_rank, MSG_SEND_APPLICATION_MESSAGE, msg);
}


















