
#pragma once

#include <stddef.h>
#include <set>
#include <vector>

#include "portfolio_solver_interface.hpp"
#include "app/sat/data/clause.hpp"
#include "app/sat/data/definitions.hpp"
#include "kissat/src/kissat.h"
#include "util/sys/threading.hpp"

struct kissat;
struct SolverSetup;
struct SolverStatistics;

class Kissat : public PortfolioSolverInterface {

private:
	kissat* solver;
	bool seedSet = false;
    int numVars = 0;

    LearnedClauseCallback callback;
    std::vector<int> learntClauseBuffer;
	Mallob::Clause learntClause;
    std::vector<int> producedClause;

	//For shared equivalence sweeping
	std::vector<int> learntEquivalenceBuffer;    //transfer a single equivalence from kissat to Mallob::Kissat
    std::vector<int> producedEquivalenceBuffer;  //transfer a single equivalence from Mallob::Kissat to kissat
    std::vector<int> stored_equivalences_to_share; //accumulate equivalences from export for the next sharing
    std::vector<int> stored_equivalences_to_import; //accumulate equivalences from sharing for the next import
	const int MAX_STORED_EQUIVALENCES = 1000;
	const int MAX_STORED_EQUIVALENCES_SIZE = MAX_STORED_EQUIVALENCES * 2;
	friend class SweepJob;

	bool interruptionInitialized = false;
    bool interrupted = false;
    unsigned int glueLimit;

	std::vector<signed char> initialVariablePhases;
	bool initialVariablePhasesLocked = false;

	std::vector<int> preprocessedFormula;
	int nbPreprocessedVariables {0};
	int nbPreprocessedClausesReceived {0};
	int nbPreprocessedClausesAdvertised {0};

public:
	Kissat(const SolverSetup& setup);
	 ~Kissat();

	// Add a (list of) permanent clause(s) to the formula
	void addLiteral(int lit) override;

	void diversify(int seed) override;
	void setPhase(const int var, const bool phase) override;

	// Solve the formula with a given set of assumptions
	SatResult solve(size_t numAssumptions, const int* assumptions) override;

	void configureBoundedVariableAddition();

	void setSolverInterrupt() override;
	void unsetSolverInterrupt() override;

	std::vector<int> getSolution() override;
	std::set<int> getFailedAssumptions() override;

	// Set a function that should be called for each learned clause
	void setLearnedClauseCallback(const LearnedClauseCallback& callback) override;

	// Set a function that should be called for each learned equivalence by sweeping
	void activateLearnedEquivalenceCallbacks();

	
	// Get the number of variables of the formula
	int getVariablesCount() override;

	int getNumOriginalDiversifications() override;
	
	// Get a variable suitable for search splitting
	int getSplittingVariable() override;

	// Get solver statistics
	void writeStatistics(SolverStatistics& stats) override;

	bool supportsIncrementalSat() override {return false;}
	bool exportsConditionalClauses() override {return false;}

	void cleanUp() override;

	void reconstructSolutionFromPreprocessing(std::vector<int>& model);

    friend void produce_clause(void* state, int size, int glue);
	friend void produce_equivalence(void *state);

    friend void consume_clause(void* state, int** clause, int* size, int* lbd);
	friend void consume_equivalence(void* state, int** equivalence);

    friend bool begin_formula_report(void* state, int vars, int cls);
    friend void report_preprocessed_lit(void* state, int lit);
    friend int terminate_callback(void* state);


	//Pass-through to access kissat_set_option
	void set_option(const std::string &option_name, int value);




private:
    void produceClause(int size, int lbd);
	void produceEquivalence();

    void consumeClause(int** clause, int* size, int* lbd);
	void consumeEquivalence(int** equivalence);

    bool isPreprocessingAcceptable(int vars, int cls);
    void addLiteralFromPreprocessing(int lit);

    bool shouldTerminate();

};
