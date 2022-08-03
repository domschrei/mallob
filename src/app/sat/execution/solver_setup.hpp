
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
        int numOriginalClauses;

	int minNumChunksPerSolver;
	int numBufferedClsGenerations;

	// SAT Solving settings

	// In any case, these bounds MUST be fulfilled for a clause to be exported
	unsigned int strictClauseLengthLimit;
	unsigned int strictLbdLimit;
	// These bounds must be fulfilled for a clause to be considered "high quality".
	// Depending on the solver, this may imply that such a clause is exported
	// while others are not. 
	unsigned int qualityClauseLengthLimit;
	unsigned int qualityLbdLimit;

	size_t clauseBaseBufferSize;
	size_t anticipatedLitsToImportPerCycle;

	bool skipClauseSharingDiagonally;
	bool certifiedUnsat;
        int maxNumSolvers;
        std::string proofDir;
};
