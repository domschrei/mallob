/*
 * Lingeling.h
 *
 *  Created on: Nov 11, 2014
 *      Author: balyo
 */

#ifndef GLUCOSE_H_
#define GLUCOSE_H_

#include "portfolio_solver_interface.hpp"
#include "util/sys/threading.hpp"
#include "app/sat/hordesat/utilities/logging_interface.hpp"

#include "app/sat/hordesat/glucose/simp/SimpSolver.h"

#include <map>

class MGlucose : SimpSolver, public PortfolioSolverInterface {

private:
	std::string name;
	int stopSolver;
	LearnedClauseCallback* learnedClauseCallback;
	int glueLimit;
	Mutex clauseAddMutex;
	int maxvar;
    
	// Friends: Callbacks for Lingeling and logging inside these callbacks
	friend void slog(PortfolioSolverInterface* slv, int verbosityLevel, const char* fmt, ...);

	// clause addition
	vector<vector<int> > clausesToAdd;
	vector<vector<int> > learnedClausesToAdd;
	vector<int> unitsToAdd;
	vector<int> assumptions;
	int* unitsBuffer;
	size_t unitsBufferSize;
	int* clsBuffer;
	size_t clsBufferSize;
    
    volatile bool suspendSolver;
    Mutex suspendMutex;
    ConditionVariable suspendCond;

	int numDiversifications;

public:
	MGlucose(LoggingInterface& logger, int globalId, int localId, std::string jobName);
	 ~MGlucose() override;

	// Add a (list of) permanent clause(s) to the formula
	void addLiteral(int lit) override;

	void diversify(int rank, int size) override;
	void setPhase(const int var, const bool phase) override;

	// Solve the formula with a given set of assumptions
	SatResult solve(const vector<int>& assumptions) override;

	void setSolverInterrupt() override;
	void unsetSolverInterrupt() override;
    void setSolverSuspend() override;
    void unsetSolverSuspend() override;

	vector<int> getSolution() override;
	set<int> getFailedAssumptions() override;

	// Add a learned clause to the formula
	// The learned clauses might be added later or possibly never
	void addLearnedClause(const int* begin, int size) override;

	// Set a function that should be called for each learned clause
	void setLearnedClauseCallback(LearnedClauseCallback* callback) override;

	// Request the solver to produce more clauses
	void increaseClauseProduction() override;
	
	// Get the number of variables of the formula
	int getVariablesCount() override;

	int getNumOriginalDiversifications() override;
	
	// Get a variable suitable for search splitting
	int getSplittingVariable() override;

	// Get solver statistics
	SolvingStatistics getStatistics() override;

private:
	bool interrupt();

    
};

#endif /* LINGELING_H_ */
