/*
 * Lingeling.h
 *
 *  Created on: Nov 11, 2014
 *      Author: balyo
 */

#ifndef LINGELING_H_
#define LINGELING_H_

#include "../utilities/SatUtils.h"
#include "PortfolioSolverInterface.h"
#include "../utilities/Threading.h"
#include "../utilities/logging_interface.h"

#include <map>

struct LGL;

class Lingeling: public PortfolioSolverInterface {

private:
	static Mutex timeCallbackLock;

	LoggingInterface& logger;
	LGL* solver;
	std::string name;
	int stopSolver;
	LearnedClauseCallback* callback;
	int glueLimit;
	Mutex clauseAddMutex;
	int myId;
	int maxvar;
	double lastTermCallbackTime;
    
	// callback friends
	friend int termCallback(void* solverPtr);
	friend void produce(void* sp, int* cls, int glue);
	friend void produceUnit(void* sp, int lit);
	friend void consumeUnits(void* sp, int** start, int** end);
	friend void consumeCls(void* sp, int** clause, int* glue);
	friend void slog(Lingeling* lgl, int verbosityLevel, const char* fmt, ...);

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

public:

	// Load formula from a given dimacs file, return false if failed
	bool loadFormula(const char* filename);

	// Get the number of variables of the formula
	int getVariablesCount();

	// Get a variable suitable for search splitting
	int getSplittingVariable();

	// Set initial phase for a given variable
	void setPhase(const int var, const bool phase);

    // Suspend the SAT solver DURING its execution, freeing up computational resources for other threads
    void setSolverSuspend();
    void unsetSolverSuspend();

	// Solve the formula with a given set of assumptions
	SatResult solve(const vector<int>& assumptions);

	vector<int> getSolution();
	set<int> getFailedAssumptions();

	// Add a (list of) permanent clause(s) to the formula
	void addLiteral(int lit);
	void addClause(vector<int>& clause);
	void addClauses(vector<vector<int> >& clauses);
	void addInitialClauses(vector<vector<int> >& clauses);
	void addClauses(const vector<int>& clauses);
	void addInitialClauses(const vector<int>& clauses);

	// Add a (list of) learned clause(s) to the formula
	// The learned clauses might be added later or possibly never
	void addLearnedClause(vector<int>& clauses);
	void addLearnedClauses(vector<vector<int> >& clauses);

	// Set a function that should be called for each learned clause
	void setLearnedClauseCallback(LearnedClauseCallback* callback, int solverId);

	// Request the solver to produce more clauses
	void increaseClauseProduction();

	// Get solver statistics
	SolvingStatistics getStatistics();

	void diversify(int rank, int size);

	// Interrupt the SAT solving, so it can be started again with new assumptions and added clauses
	void setSolverInterrupt();
	void unsetSolverInterrupt();
    
	Lingeling(LoggingInterface& logger, int solverId);
	 ~Lingeling();
};

#endif /* LINGELING_H_ */
