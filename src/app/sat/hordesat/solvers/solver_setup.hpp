
#pragma once

#include <string>

#include "util/logger.hpp"

struct SolverSetup {

	// General important fields
	Logger* logger;
	int globalId;
	int localId; 
	std::string jobname; 
	int diversificationIndex;
	bool isJobIncremental;
	bool doIncrementalSolving;
	bool hasPseudoincrementalSolvers;
	char solverType;
	int solverRevision;
	
	int minNumChunksPerSolver;
	int numBufferedClsGenerations;

	// SAT Solving settings

	// In any case, these bounds MUST be fulfilled for a clause to be exported
	unsigned int hardMaxClauseLength;
	unsigned int hardInitialMaxLbd;
	unsigned int hardFinalMaxLbd;
	// These bounds may not be fulfilled in case the solver deems the clause very good
	// due to other observations
	unsigned int softMaxClauseLength;
	unsigned int softInitialMaxLbd;
	unsigned int softFinalMaxLbd;

	size_t clauseBaseBufferSize;
	size_t anticipatedLitsToImportPerCycle;
};
