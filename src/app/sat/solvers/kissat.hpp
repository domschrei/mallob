
#pragma once

#include <stddef.h>
#include <set>
#include <vector>

#include "app/sat/proof/lrat_op.hpp"
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
	//Sweeping
	bool is_sweeper = false;
	bool is_congruencer = false; //subtype of sweeper, that does congruence closure instead of sweeping
	int representative_localId = 0;
	friend class SweepJob; //fwd
	std::vector<int> eq_up_buffer;    //transfer a single equivalence from C to C++

	std::vector<int> eqs_from_broadcast_queued; //equivalences that came from the broadcast, but are not yet shown to the solver
	std::vector<int> units_from_broadcast_queued;

    std::vector<int> eqs_from_broadcast;  //equivalences that are currently shown to the solver, originating from broadcast
	std::vector<int> units_from_broadcast;

    std::vector<int> eqs_to_share;    //accumulate exported equivalences for sharing
	std::vector<int> units_to_share;
	std::mutex sweep_export_mutex; //when exporting data from the solver to Mallob, need to lock them when extracting them for global sharing, otherwise the solver threads might continue concurrently pushing new data onto them
	std::vector<int> work_received_from_steal;

	std::atomic_flag steal_victim_lock = ATOMIC_FLAG_INIT;

	std::atomic_bool sweeper_is_idle = false; //current idle-status
	std::atomic_bool sweeper_longterm_idle = false; //whether the sweeper remained idle through a whole period of checking

	// bool has_reported_sweep_dimacs = false;

	std::atomic_int sweep_import_round{0};
	std::atomic_int sweep_EQS_index{0};
	std::atomic_int sweep_EQS_size{0};
	std::atomic_int sweep_UNITS_index{0};
	std::atomic_int sweep_UNITS_size{0};
	// int sweep_unread_EQS_count{0};

	//New Import Version with dedicated vectors per round
	int curr_eq_round{0};
	int curr_eq_index{0};
	int curr_unit_round{0};
	int curr_unit_index{0};


	struct shweep_statistics sweep_stats;

	static constexpr int WARN_ON_REPEATED_MISSED_TERMINATION=32;
	int count_repeated_missed_termination=0;
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
    // friend void consume_clause(void* state, int** clause, int* size, int* lbd);
    friend void consume_clause(void* state, int** clause, int* size, int* lbd, unsigned long* id, unsigned char* sig);
	friend void on_drup_derivation(void* state, const int* lits, int nbLits, int glue);
    friend void on_lrup_import(void* state, unsigned long id, const int* lits, int nbLits, const uint8_t* sigData);
    friend void on_drup_deletion(void* state, const int* lits, int nbLits);

	friend void report_database_lit(void *state, int lit);

	//Preprocessing
    friend bool begin_formula_report(void* state, int vars, int cls);
    friend void report_preprocessed_lit(void* state, int lit);
    friend int terminate_callback(void* state);

	//Shared Sweeping / SWEEP App
	friend void sweep_export_eq(void *state);
	friend void sweep_export_unit(void *state, int unit);
	// friend void sweep_import_eqs(void* state, int** equivalences, int *eqs_size);
	// friend void sweep_import_units(void *state, int **units, int *unit_count);
	// void sweepSetReportingPtr(std::shared_ptr<std::atomic<int>> field);
	void setToSweeper();
	// void triggerSweepTerminate();
	void triggerSweepTerminate(bool solver_does_single_iterations);
	void setRepresentativeLocalId(int localId);
	// bool hasReportedSweepDimacs() const;
	// shweep_statistics getSweepStats();

	//Pass-through
	bool set_option(const std::string &option_name, int value);


private:
    void produceClause(int size, int lbd);
    // void consumeClause(int** clause, int* size, int* lbd);
	void consumeClause(int** clause, int* size, int* lbd, unsigned long* id, unsigned char* sig);
	void processProofLine(LratOp&& op);

	void sweepSetReportCallback();
    bool isPreprocessingAcceptable(int vars, int cls);
    void addLiteralFromPreprocessing(int lit);
	shweep_statistics fetchSweepStats();


    bool shouldTerminate();

	//Shared Sweeping
	void sweepExportEq();
	void sweepExportUnit(int unit);
	void sweepImportEqs(int** equivalences, int *eqs_size);
	void sweepImportUnits(int **units, int *unit_count);
    // void addLiteralToShweepJob(int lit);

	void sweepSetExportCallbacks();
	// void shweepSetWorkstealingCallback(void* SweepJob_state, void (*search_callback)(void *SweepJob_state, unsigned **work, int *work_size, int local_id));

	// void startSweepAppCallback();

};
