/*
 * Cadical.hpp
 *
 *  Created on: Jun 26, 2020
 *      Author: schick
 */

#ifndef MSCHICK_CADICAL_H_
#define MSCHICK_CADICAL_H_

#include <map>

#include "portfolio_solver_interface.hpp"

#include "util/sys/threading.hpp"
#include "app/sat/hordesat/utilities/logging_interface.hpp"

#include "app/sat/hordesat/solvers/cadical_interface.hpp"
#include "app/sat/hordesat/solvers/cadical_terminator.hpp"
#include "app/sat/hordesat/solvers/cadical_learner.hpp"

class Cadical : public PortfolioSolverInterface {

private:
	CaDiCaL::Solver solver;

	Mutex learnMutex;

	vector<vector<int> > learnedClauses;
	vector<int> assumptions;

	HordeTerminator terminator;
    HordeLearner learner;

	bool seedSet = false;

public:
	Cadical(const SolverSetup& setup);

	// Add a (list of) permanent clause(s) to the formula
	void addLiteral(int lit) override;

	void diversify(int seed) override;
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
};

#endif /* CADICAL_H_ */
