/*
 * MiniSat.h
 *
 *  Created on: Oct 9, 2014
 *      Author: balyo
 */

#ifndef MINISAT_H_
#define MINISAT_H_

#include "PortfolioSolverInterface.h"
#include "../utilities/Threading.h"
using namespace std;

#define CLS_COUNT_INTERRUPT_LIMIT 300

// some forward declatarations for Minisat
namespace Minisat {
	class Solver;
	class Lit;
	template<class T, class _Size> class vec;
}


class MiniSat : public PortfolioSolverInterface {

private:
	Minisat::Solver *solver;
	vector< vector<int> > learnedClausesToAdd;
	vector< vector<int> > clausesToAdd;
	Mutex clauseAddingLock;
	int myId;
	LearnedClauseCallback* callback;
	int learnedLimit;
	friend void miniLearnCallback(const Minisat::vec<Minisat::Lit,int>& cls, void* issuer);

public:

	bool loadFormula(const char* filename);
	//Get the number of variables of the formula
	int getVariablesCount();
	// Get a variable suitable for search splitting
	int getSplittingVariable();
	// Set initial phase for a given variable
	void setPhase(const int var, const bool phase);
	// Interrupt the SAT solving, so it can be started again with new assumptions
	void setSolverInterrupt();
	void unsetSolverInterrupt();
    
    inline void setSolverSuspend() {};
    inline void unsetSolverSuspend() {};

	// Solve the formula with a given set of assumptions
	// return 10 for SAT, 20 for UNSAT, 0 for UNKNOWN
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
	void addLearnedClause(const int* begin, int size);
	void addLearnedClauses(vector<vector<int> >& clauses);

	// Set a function that should be called for each learned clause
	void setLearnedClauseCallback(LearnedClauseCallback* callback, int solverId);

	// Request the solver to produce more clauses
	void increaseClauseProduction();

	// Get solver statistics
	SolvingStatistics getStatistics();

	// Diversify
	void diversify(int rank, int size);
	int getNumOriginalDiversifications();

	// constructor
	MiniSat();
	// destructor
	virtual ~MiniSat();

};


#endif /* MINISAT_H_ */
