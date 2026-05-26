
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

enum class SweepStealType {
	MPI   = 1,
	Local = 2,
};


struct SweepStealInfo {
	int		nr;
	SweepStealType stealtype;
	int		size;
	float	t_submit;
	float	t_receive;
	float   t_read;
	int		round;
};

class Kissat : public PortfolioSolverInterface {

private:
	kissat* solver;
	bool seedSet = false;
    int numVars = 0;

    LearnedClauseCallback callback;
    std::vector<int> learntClauseBuffer;
	Mallob::Clause learntClause;
    std::vector<int> producedClause;



	//#################################################################################################
	//For MallobSweep
	bool is_sweeper = false;
	int representative_localId = 0;
	friend class SweepJob; //fwd decl

	//transfer a single equivalence from C to C++
	std::vector<int> eq_up_buffer;

	//accumulate unit and equivalences for sharing
	//when exporting data from the solver to Mallob, need to lock the vector when extracting for global sharing,
	//otherwise the solver threads might concurrently push new data into the std::vector while we are reading it
	std::vector<int> units_to_share;
    std::vector<int> eqs_to_share;
	std::mutex sweep_export_mutex;

	//Work received by stealing from others
	//This vector is allocated and controlled on the C++ level, but accessible and operated on also within the kissat solver
	//on the C level. The kissat solver thus trusts that this array is always available and never suddenly reallocated.
	std::vector<int> work_received_from_steal;

	//We make sure that only one other solver can steal from this one at a time
	std::atomic_flag steal_victim_lock = ATOMIC_FLAG_INIT;

	//Tracking the status of the solver
	std::atomic_bool sweeper_is_idle = false;
	std::atomic_bool sweeper_longterm_idle = false;

	//The solver tracks which data it already imported that arrived via a global sharing round
	int curr_eq_round{0};
	int curr_eq_index{0};
	int curr_unit_round{0};
	int curr_unit_index{0};

	//Some more detailed information collection on stealing, for debugging
	int attempted_steals = 0;
	std::vector<SweepStealInfo> steal_records{};
	struct shweep_statistics sweep_stats;

	static constexpr int WARN_ON_REPEATED_MISSED_TERMINATION=32;
	int count_repeated_missed_termination=0;
	//##################################################################################################






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
	void triggerSweepTerminate();
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

	void sweepSetFormulaReportCallback();
    bool isPreprocessingAcceptable(int vars, int cls);
    void addLiteralFromPreprocessing(int lit);
	shweep_statistics fetchSweepStats();


    bool shouldTerminate();

	//Shared Sweeping
	void sweepExportEq();
	void sweepExportUnit(int eunit);
	void sweepImportEqs(int** equivalences, int *eqs_size);
	void sweepImportUnits(int **units, int *unit_count);
    // void addLiteralToShweepJob(int lit);

	void sweepSetExportCallbacks();
	// void shweepSetWorkstealingCallback(void* SweepJob_state, void (*search_callback)(void *SweepJob_state, unsigned **work, int *work_size, int local_id));

	// void startSweepAppCallback();

};
