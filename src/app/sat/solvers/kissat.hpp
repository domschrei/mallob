
#pragma once

#include <stddef.h>
#include <set>
#include <vector>

#include "portfolio_solver_interface.hpp"
#include "app/sat/data/clause.hpp"
#include "app/sat/data/definitions.hpp"

extern "C" {
#include "kissat/src/kissat.h"
}
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

	//Shweep
	std::vector<int> eq_up_buffer;    //transfer a single equivalence from C to C++


	std::vector<int> eqs_from_broadcast_queued; //equivalences that came from the broadcast, but are not yet shown to the solver
	std::vector<int> units_from_broadcast_queued;

    std::vector<int> eqs_from_broadcast;  //equivalences that are currently shown to the solver, originating from broadcast
	std::vector<int> units_from_broadcast;


    std::vector<int> eqs_to_share;    //accumulate exported equivalences for sharing
	std::vector<int> units_to_share;

	// bool shweep_eq_imports_available;
	// bool shweep_unit_imports_available;
	// const int MAX_SHWEEP_STORAGE_SIZE = 10000;

	friend class SweepJob;

	// std::vector<int> work_stolen_from_local_solver;
	std::vector<int> work_received_from_steal;

	bool shweep_is_idle;
	// std::vector<char> stolen_done;


	std::vector<int> formulaForShweeping;
	// \Shweep

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
	void shweep_set_importexport_callbacks();

	
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

	//Distributed Shared Sweeping
	friend void shweep_export_eq(void *state);
	friend void shweep_export_unit(void *state, int unit);

	friend void shweep_import_eqs(void* state, int** equivalences, int *eqs_size);
	friend void shweep_import_units(void *state, int **units, int *unit_count);



	friend void report_database_lit(void *state, int lit);


	void shweep_set_workstealing_callback(void* SweepJob_state, void (*search_callback)(void *SweepJob_state, unsigned **work, int *work_size, int local_id));



	//
    friend bool begin_formula_report(void* state, int vars, int cls);
    friend void report_preprocessed_lit(void* state, int lit);
    friend int terminate_callback(void* state);


	//Pass-through to access kissat_set_option
	void set_option(const std::string &option_name, int value);




private:
    void produceClause(int size, int lbd);
    void consumeClause(int** clause, int* size, int* lbd);


	// void shweep_solverSearchesWork(unsigned **work, unsigned *size);

    bool isPreprocessingAcceptable(int vars, int cls);
    void addLiteralFromPreprocessing(int lit);

	//Shweep
	void shweepExportEq();
	void shweepExportUnit(int unit);

	void shweepImportEqs(int** equivalences, int *eqs_size);
	void shweepImportUnits(int **units, int *unit_count);

    void addLiteralToShweepJob(int lit);

    bool shouldTerminate();

};
