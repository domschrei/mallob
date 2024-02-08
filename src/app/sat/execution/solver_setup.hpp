
#pragma once

#include <string>

#include "app/sat/data/portfolio_sequence.hpp"
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
	PortfolioSequence::Flavour flavour;

	// In any case, these bounds MUST be fulfilled for a clause to be exported
	unsigned int strictMaxLitsPerClause;
	unsigned int strictLbdLimit;
	// These bounds must be fulfilled for a clause to be considered "high quality".
	// Depending on the solver, this may imply that such a clause is exported
	// while others are not. 
	unsigned int qualityMaxLitsPerClause;
	unsigned int qualityLbdLimit;

	bool shareClauses; // exporting clauses to other solvers?
	size_t clauseBaseBufferSize;
	size_t anticipatedLitsToImportPerCycle;
	bool resetLbdBeforeImport {false};
	bool incrementLbdBeforeImport {false};
	bool randomizeLbdBeforeImport {false};

	bool adaptiveImportManager;
	bool skipClauseSharingDiagonally;

	// This solver emits proof information, either for on-the-fly checking or for proof production.
	bool certifiedUnsat;
	// This solver's proof information is checked on-the-fly.
	bool onTheFlyChecking;
	// This solver is NOT allowed to participate in UNSAT solving by exporting clauses or reporting UNSAT,
	// usually because it does not emit proof information while other solvers do.
	bool avoidUnsatParticipation;

	int maxNumSolvers;
	std::string proofDir;
	std::string sigFormula;
};
