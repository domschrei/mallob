
#pragma once

#include "portfolio_solver_interface.hpp"

struct kissat;

class Kissat : public PortfolioSolverInterface {

private:
	kissat* solver;
	bool seedSet = false;
    int numVars = 0;

    LearnedClauseCallback callback;
    int learntClauseBuffer[100];
	Clause learntClause;
    std::vector<int> producedClause;

    bool interrupted = false;
    bool suspended = false;
    Mutex suspendMutex;
    ConditionVariable suspendCondVar;
    unsigned int glueLimit;

	std::vector<signed char> initialVariablePhases;
	bool initialVariablePhasesLocked = false;


public:
	Kissat(const SolverSetup& setup);
	 ~Kissat();

	// Add a (list of) permanent clause(s) to the formula
	void addLiteral(int lit) override;

	void diversify(int seed) override;
	void setPhase(const int var, const bool phase) override;

	// Solve the formula with a given set of assumptions
	SatResult solve(size_t numAssumptions, const int* assumptions) override;

	void setSolverInterrupt() override;
	void unsetSolverInterrupt() override;
    void setSolverSuspend() override;
    void unsetSolverSuspend() override;

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

    friend void produce_clause(void* state, int size, int glue);
    friend void consume_clause(void* state, int** clause, int* size, int* lbd);
    friend int terminate_callback(void* state);

private:
    void produceClause(int size, int lbd);
    void consumeClause(int** clause, int* size, int* lbd);
    bool shouldTerminate();

};
