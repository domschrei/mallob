//
// Created by nicco on 11/5/26.
//

#include "musEngine.hpp"

std::string MusEngine::getSolverChoicesStr() {
	return "Cr";
	//The MUS engine uses incremental Cadical and Rotator solvers  (incremental is signalled via uppercase)
}
int MusEngine::getNumSolvers(int numThreads) {
	return 2 * numThreads;
	//For each available physical thread/core we allocate two solvers (Cadical and Rotator, see the "cr" string)
	//The ScatterEngine makes sure that only numThreads of them run at any given time.
}

std::vector<ScatterEngine::JobToSolverMapping> MusEngine::getJobToSolverMappingDefinitions() {
	return {
		{KICK_CANDIDATE_CLAUSE, SolverBackendType::CADICAL},
		{ROTATE_MODEL_AROUND_CLAUSE, SolverBackendType::ROTATOR}
	};
}

MusEngine::MusEngine(const Parameters& params, const SatProcessConfig& config, Logger& loggingInterface)
	: ScatterEngine(params, config, loggingInterface)
{
	ScatterEngine::init();
}

