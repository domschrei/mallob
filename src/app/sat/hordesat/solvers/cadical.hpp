/*
 * Cadical.hpp
 *
 *  Created on: Jun 26, 2020
 *      Author: schick
 */

#ifndef MSCHICK_CADICAL_H_
#define MSCHICK_CADICAL_H_

#include "portfolio_solver_interface.hpp"

#include "util/sys/threading.hpp"
#include "util/logger.hpp"

#include "app/sat/hordesat/solvers/cadical_interface.hpp"
#include "app/sat/hordesat/solvers/cadical_terminator.hpp"
#include "app/sat/hordesat/solvers/cadical_learner.hpp"
#include "app/sat/hordesat/solvers/cadical_learn_source.hpp"

class Cadical : public PortfolioSolverInterface {

private:
	std::unique_ptr<CaDiCaL::Solver> solver;

	Mutex learnMutex;

	std::vector<std::vector<int> > learnedClauses;
	std::vector<int> assumptions;

	HordeTerminator terminator;
    HordeLearner learner;
	MallobLearnSource learnSource;

	bool seedSet = false;

public:
	Cadical(const SolverSetup& setup);
	 ~Cadical();

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
	void writeStatistics(SolvingStatistics& stats) override;

	bool supportsIncrementalSat() override {return true;}
	bool exportsConditionalClauses() override {return false;}
};

#endif /* CADICAL_H_ */
