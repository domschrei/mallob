
#pragma once

#include <string>

#include "app/sat/data/portfolio_sequence.hpp"
#include "util/logger.hpp"
#include "util/random.hpp"

class LratConnector; // fwd

struct SolverSetup {


	// General important fields

	Logger* logger {nullptr};
	int globalId {0};
	int localId {0};
	std::string jobname;
	std::string profilingBaseDir; 
	int profilingLevel {-1};


	// Properties of the SAT problem

	int numVars {0};
	int numOriginalClauses {0};
	int maxNumSolvers {1};
	bool isJobIncremental {false};
	bool hasPseudoincrementalSolvers {false};
	int solverRevision {0};


	// Solver configuration and diversification

	char solverType;
	bool doIncrementalSolving {false};
	int diversificationIndex {0};

	bool diversifyNoise {false};
	int decayDistribution {0};
	int decayMean {50};
	int decayStddev {3};
	int decayMin {1};
	int decayMax {200};

	int diversifyReduce {0};
	int reduceMin {300};
	int reduceMax {980};
	int reduceDelta {100};
	int reduceMean {700};
	int reduceStddev {150};

	bool diversifyNative {false};
	bool diversifyFanOut {false};
	bool diversifyInitShuffle {false};
	enum EliminationSetting {
		ALLOW_ALL, DISABLE_SOME, DISABLE_MOST, DISABLE_ALL
	} eliminationSetting {ALLOW_ALL};
	PortfolioSequence::Flavour flavour {PortfolioSequence::DEFAULT};

	// Clause export

	bool exportClauses {true}; // exporting clauses to other solvers?
	bool skipClauseSharingDiagonally {false};
	// In any case, these bounds MUST be fulfilled for a clause to be exported
	unsigned int strictMaxLitsPerClause {255};
	unsigned int strictLbdLimit {255};
	// These bounds must be fulfilled for a clause to be considered "high quality".
	// Depending on the solver, this may imply that such a clause is exported
	// while others are not. 
	unsigned int qualityMaxLitsPerClause {255};
	unsigned int qualityLbdLimit {255};
	unsigned int freeMaxLitsPerClause {255};
	size_t clauseBaseBufferSize {1000};


	// Clause import

	int minImportChunksPerSolver {10};
	int numBufferedClsGenerations {10};
	size_t anticipatedLitsToImportPerCycle {1000};
	bool resetLbdBeforeImport {false};
	bool incrementLbdBeforeImport {false};
	int randomizeLbdBeforeImport;
	bool adaptiveImportManager;


	// Certified UNSAT and proof production

	// This solver emits proof information, either for on-the-fly checking or for proof production.
	bool certifiedUnsat {false};
	// This solver's proof information is checked on-the-fly.
	bool onTheFlyChecking {false};
	// If on-the-fly checking is enabled: this solver also seeks to have a found satisfying assignment checked.
	bool onTheFlyCheckModel {false};
	// If non-null, use this LratConnector instance for checking a model;
	// if null && onTheFlyCheckModel, then *create* a model-checking LRAT connector instance yourself (also to use for others).
	LratConnector* modelCheckingLratConnector {nullptr};
	bool owningModelCheckingLratConnector {false};
	// This solver is NOT allowed to participate in UNSAT solving by exporting clauses or reporting UNSAT,
	// usually because it does not emit proof information while other solvers do.
	bool avoidUnsatParticipation {false};
	// Directory to write proofs to
	std::string proofDir;
	// Signature of the formula to process - to be validated by the on-the-fly checker
	std::string sigFormula;
	int nbSkippedIdEpochs {0};


	// Optimization and theories

	std::vector<std::pair<long, int>> objectiveFunction;
};
