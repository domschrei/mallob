
#include "sweep_job.hpp"


#include "app/job.hpp"
#include "app/job_tree.hpp"
#include "comm/job_tree_all_reduction.hpp"
#include "util/logger.hpp"


SweepJob::SweepJob(const Parameters& params, const JobSetup& setup, AppMessageTable& table)
    : Job(params, setup, table) {
}


void SweepJob::appl_start() {
	printf("ß appl_start\n");
	_my_rank = getJobTree().getRank();
	_my_index = getJobTree().getIndex();
	_is_root = getJobTree().isRoot();
	printf("ß Rank %i, Index %i, is root %i, Children %i, Demand %i\n", _my_rank, _my_index, _is_root, getJobTree().getNumChildren(), getDemand());
    _data = getSerializedDescription(0)->data();

	const JobDescription& desc = getDescription();

	SolverSetup setup;
	setup.logger = &Logger::getMainInstance();
	setup.jobname = "swissat-"+to_string(_my_index);
	// setup.numVars = desc.getFormulaPayload(0)[0];
	// setup.numOriginalClauses = desc.getFormulaPayload(0)[1];
	setup.numVars = desc.getAppConfiguration().fixedSizeEntryToInt("__NV");
	setup.numOriginalClauses = desc.getAppConfiguration().fixedSizeEntryToInt("__NC");
	// setup.numOriginalClauses = desc.getAppConfiguration().fixedSizeEntryToInt("__NC");
	printf("ß [%i] Payload: %i vars, %i clauses \n", _my_index, setup.numVars, setup.numOriginalClauses);

	_swissat.reset(new Kissat(setup));

	_swissat->set_option_externally("mallob_solver_id", _my_index);
	_swissat->set_option_externally("mallob_solver_count", 4); //hardcode 4 solvers for now
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
	// _solved_status = 1;
	_internal_result.id = getId();
    _internal_result.revision = getRevision();
    _internal_result.result=res;
	auto dummy_solution = std::vector<int>(1,0);
	_internal_result.setSolutionToSerialize((int*)(dummy_solution.data()), dummy_solution.size());
}

