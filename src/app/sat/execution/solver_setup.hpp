
#pragma once

#include <string>

#include "util/logger.hpp"
#include "util/random.hpp"

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
	int numVars;

	int minNumChunksPerSolver;
	int numBufferedClsGenerations;
	bool diversifyNoise;
	bool diversifyNative;
	bool diversifyFanOut;
	bool diversifyInitShuffle;
	enum EliminationSetting {
		ALLOW_ALL, DISABLE_SOME, DISABLE_MOST, DISABLE_ALL
	} eliminationSetting;

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
	bool resetLbdBeforeImport {false};
	bool incrementLbdBeforeImport {false};
	bool randomizeLbdBeforeImport {false};

	bool adaptiveImportManager;
	bool skipClauseSharingDiagonally;
	bool certifiedUnsat;
	int maxNumSolvers;
	std::string proofDir;
};
