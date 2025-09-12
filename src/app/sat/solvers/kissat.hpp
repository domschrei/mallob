
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
struct sweeper;

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
	std::vector<int> pass_eq_up_buffer;    //transfer a single equivalence up (from kissat to Mallob::Kissat)
    // std::vector<int> producedEquivalenceBuffer;  //transfer a single equivalence down (from Mallob::Kissat to kissat)
    std::vector<int> eqs_to_share; //accumulate exported equivalences from local solver, to share
    std::vector<int> eqs_to_pass_down; //accumulate share-received equivalences, to import in local solver
	const int MAX_STORED_EQUIVALENCES = 10000;
	const int MAX_STORED_EQUIVALENCES_SIZE = MAX_STORED_EQUIVALENCES * 2;
	friend class SweepJob;
	//Update stuff for sweep sharing
	std::vector<unsigned> work_stolen_locally;
	std::vector<unsigned> my_work;
	// std::vector<char> stolen_done;


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
	void activateEqImportExportCallbacks();

	
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
    friend void consume_clause(void* state, int** clause, int* size, int* lbd);

	friend void pass_eq_up(void *state);
	friend void pass_eqs_down(void* state, int** equivalence, unsigned *eq_count);

	//Distributed Shared Sweeping
	// friend void shweep_ts_stolen_work(void *state, unsigned **work, unsigned *size);
	// friend void shweep_ts_stolen_done(void *state, char **done, unsigned *size);
	// friend void shweep_solver_searches_work(void *JSstate, unsigned **work, unsigned *size);

	void shweep_SetSearchWorkCallback(void* SweepJob_state, void (*search_callback)(void *SweepJob_state, unsigned **work, unsigned *work_size));



    friend bool begin_formula_report(void* state, int vars, int cls);
    friend void report_preprocessed_lit(void* state, int lit);
    friend int terminate_callback(void* state);


	//Pass-through to access kissat_set_option
	void set_option(const std::string &option_name, int value);




private:
    void produceClause(int size, int lbd);
    void consumeClause(int** clause, int* size, int* lbd);

	void passEqUp();
	void passEqsDown(int** equivalence, unsigned *eq_count);

	//Shweep
	// void shweep_solverSearchesWork(unsigned **work, unsigned *size);

    bool isPreprocessingAcceptable(int vars, int cls);
    void addLiteralFromPreprocessing(int lit);

    bool shouldTerminate();

};
