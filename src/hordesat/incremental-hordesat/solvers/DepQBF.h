/*
 * DepQBF.h
 *
 *  Adapted by Florian Lonsing, January 2015
 *
 *  Created on: Nov 11, 2014
 *      Author: balyo
 */

#ifndef DEPQBF_H_
#define DEPQBF_H_

#include "../utilities/SatUtils.h"
#include "PortfolioSolverInterface.h"
#include "../utilities/Threading.h"

struct QDPLL;

class DepQBF: public PortfolioSolverInterface {

private:
	/* TODO: not all members are necessary for DepQBF, most of them have been
	 copied from Lingeling's class implementation. */
	QDPLL* solver;
	int stopSolver;
	LearnedClauseCallback* callback;
	/* Size limit of shared clauses. */
	int sizeLimit;
	Mutex clauseAddMutex;
	int myId;

	// callback friends
	friend void qbfproduce(void* sp, int* cls, int glue);
	friend void qbfconsumeCls(void* sp, int** clause, int* glue);

	// clause addition
	vector<vector<int> > clausesToAdd;
	vector<vector<int> > learnedClausesToAdd;
	vector<int> unitsToAdd;
	int* unitsBuffer;
	size_t unitsBufferSize;
	int* clsBuffer;
	size_t clsBufferSize;

public:

	// Load formula from a given dimacs file, return false if failed
	bool loadFormula(const char* filename);

	// Get the number of variables of the formula
	int getVariablesCount();

	// Get a variable suitable for search splitting
	int getSplittingVariable();

	// Set initial phase for a given variable
	void setPhase(const int var, const bool phase);

	// Interrupt the SAT solving, so it can be started again with new assumptions and added clauses
	void setSolverInterrupt();
	void unsetSolverInterrupt();

	// Solve the formula with a given set of assumptions
	SatResult solve(const vector<int>& assumptions);

	vector<int> getSolution();
	set<int> getFailedAssumptions();

	// Add a (list of) permanent clause(s) to the formula
	void addLiteral(int lit);
	void addClause(vector<int>& clause);
	void addClauses(vector<vector<int> >& clauses);
	void addInitialClauses(vector<vector<int> >& clauses);

	// Add a (list of) learned clause(s) to the formula
	// The learned clauses might be added later or possibly never
	void addLearnedClause(vector<int>& clauses);
	void addLearnedClauses(vector<vector<int> >& clauses);

	// Set a function that should be called for each learned clause
	void setLearnedClauseCallback(LearnedClauseCallback* callback,
			int solverId);

	// Request the solver to produce more clauses
	void increaseClauseProduction();

	// Get solver statistics
	SolvingStatistics getStatistics();

	void diversify(int rank, int size);

	DepQBF();
	~DepQBF();
};

#endif /* DEPQBF_H_ */
