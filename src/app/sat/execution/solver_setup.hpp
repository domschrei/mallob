
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


	// Properties of the SAT problem

	int numVars;
	int numOriginalClauses;
	int maxNumSolvers;
	bool isJobIncremental;
	bool hasPseudoincrementalSolvers;
	int solverRevision;


	// Solver configuration and diversification

	char solverType;
	bool doIncrementalSolving;
	int diversificationIndex;
	bool diversifyNoise;
	bool diversifyNative;
	bool diversifyFanOut;
	bool diversifyInitShuffle;
	enum EliminationSetting {
		ALLOW_ALL, DISABLE_SOME, DISABLE_MOST, DISABLE_ALL
	} eliminationSetting;
	PortfolioSequence::Flavour flavour;


	// Clause export

	bool exportClauses; // exporting clauses to other solvers?
	bool skipClauseSharingDiagonally;
	// In any case, these bounds MUST be fulfilled for a clause to be exported
	unsigned int strictMaxLitsPerClause;
	unsigned int strictLbdLimit;
	// These bounds must be fulfilled for a clause to be considered "high quality".
	// Depending on the solver, this may imply that such a clause is exported
	// while others are not. 
	unsigned int qualityMaxLitsPerClause;
	unsigned int qualityLbdLimit;
	size_t clauseBaseBufferSize;


	// Clause import

	int minImportChunksPerSolver;
	int numBufferedClsGenerations;
	size_t anticipatedLitsToImportPerCycle;
	bool resetLbdBeforeImport {false};
	bool incrementLbdBeforeImport {false};
	bool randomizeLbdBeforeImport {false};
	bool adaptiveImportManager;


	// Certified UNSAT and proof production

	// This solver emits proof information, either for on-the-fly checking or for proof production.
	bool certifiedUnsat;
	// This solver's proof information is checked on-the-fly.
	bool onTheFlyChecking;
	// This solver is NOT allowed to participate in UNSAT solving by exporting clauses or reporting UNSAT,
	// usually because it does not emit proof information while other solvers do.
	bool avoidUnsatParticipation;
	// Directory to write proofs to
	std::string proofDir;
	// Signature of the formula to process - to be validated by the on-the-fly checker
	std::string sigFormula;
};
