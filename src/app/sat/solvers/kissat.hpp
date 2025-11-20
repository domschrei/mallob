
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
// class SweepJob;


class Kissat : public PortfolioSolverInterface {

private:
	kissat* solver;
	bool seedSet = false;
    int numVars = 0;

    LearnedClauseCallback callback;
    std::vector<int> learntClauseBuffer;
	Mallob::Clause learntClause;
    std::vector<int> producedClause;



	//#################################################
	//Shweep
	bool is_sweeper = false;
	friend class SweepJob; //fwd
	std::vector<int> eq_up_buffer;    //transfer a single equivalence from C to C++

	std::vector<int> eqs_from_broadcast_queued; //equivalences that came from the broadcast, but are not yet shown to the solver
	std::vector<int> units_from_broadcast_queued;

    std::vector<int> eqs_from_broadcast;  //equivalences that are currently shown to the solver, originating from broadcast
	std::vector<int> units_from_broadcast;

    std::vector<int> eqs_to_share;    //accumulate exported equivalences for sharing
	std::vector<int> units_to_share;
	std::mutex sweep_sharing_mutex; //need to lock "eqs_to_share" and "units_to_share" when extracting them for sharing, because solver thread can be concurrently pushing new data onto them
	// std::mutex shweep_unit_mutex;

	std::vector<int> work_received_from_steal;

	bool sweeper_is_idle = false;
	std::shared_ptr<std::atomic<int>> sweepReportingLocalId;
	bool has_reported_sweep_dimacs = false;

	// std::vector<char> stolen_done;
	// std::vector<int> formulaForShweeping;
	//##################################################






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

	friend void report_database_lit(void *state, int lit);

	//Preprocessing
    friend bool begin_formula_report(void* state, int vars, int cls);
    friend void report_preprocessed_lit(void* state, int lit);
    friend int terminate_callback(void* state);

	//Shared Sweeping / SWEEP App
	friend void sweep_export_eq(void *state);
	friend void sweep_export_unit(void *state, int unit);
	friend void sweep_import_eqs(void* state, int** equivalences, int *eqs_size);
	friend void sweep_import_units(void *state, int **units, int *unit_count);
	void sweepSetReportingPtr(std::shared_ptr<std::atomic<int>> field);
	void setToSweeper();
	void setSweepTerminate();
	bool hasReportedSweepDimacs() const;

	//Pass-through
	bool set_option(const std::string &option_name, int value);


private:
    void produceClause(int size, int lbd);
    void consumeClause(int** clause, int* size, int* lbd);


	void sweepSetReportCallback();
    bool isPreprocessingAcceptable(int vars, int cls);
    void addLiteralFromPreprocessing(int lit);
	void fetchSweeperStats();

    bool shouldTerminate();

	//Shared Sweeping
	void sweepExportEq();
	void sweepExportUnit(int unit);
	void sweepImportEqs(int** equivalences, int *eqs_size);
	void sweepImportUnits(int **units, int *unit_count);
    // void addLiteralToShweepJob(int lit);

	void sweepSetImportExportCallbacks();
	// void shweepSetWorkstealingCallback(void* SweepJob_state, void (*search_callback)(void *SweepJob_state, unsigned **work, int *work_size, int local_id));

	// void startSweepAppCallback();

};
