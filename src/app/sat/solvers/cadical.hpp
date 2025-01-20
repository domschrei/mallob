/*
 * Cadical.hpp
 *
 *  Created on: Jun 26, 2020
 *      Author: schick
 */

#pragma once

#include <stddef.h>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "app/sat/proof/trusted/trusted_solving.hpp"
#include "app/sat/proof/trusted_checker_process_adapter.hpp"
#include "app/sat/solvers/optimizing_propagator.hpp"
#include "portfolio_solver_interface.hpp"
#include "util/sys/threading.hpp"
#include "util/logger.hpp"
#include "cadical/src/cadical.hpp"
#include "cadical_terminator.hpp"
#include "cadical_clause_export.hpp"
#include "cadical_clause_import.hpp"
#include "app/sat/data/definitions.hpp"

class LratConnector; // fwd
struct SolverSetup;
struct SolverStatistics;

class Cadical : public PortfolioSolverInterface {

private:
	std::unique_ptr<CaDiCaL::Solver> solver;

	Mutex learnMutex;

	std::vector<std::vector<int>> learnedClauses;
	std::vector<int> assumptions;

	CadicalTerminator terminator;
	CadicalClauseExport learner;
	CadicalClauseImport learnSource;

	bool seedSet = false;

	std::string proofFileString;
	std::string profileFileString;

	std::unique_ptr<OptimizingPropagator> optimizer;

public:
	Cadical(const SolverSetup& setup);

	// Add a (list of) permanent clause(s) to the formula
	void addLiteral(int lit) override;

	void diversify(int seed) override;
	void setPhase(const int var, const bool phase) override;

	// Solve the formula with a given set of assumptions
	SatResult solve(size_t numAssumptions, const int* assumptions) override;

	void setSolverInterrupt() override;
	void unsetSolverInterrupt() override;

	std::vector<int> getSolution() override;
	std::set<int> getFailedAssumptions() override;

	// Set a function that should be called for each learned clause
	void setLearnedClauseCallback(const LearnedClauseCallback& callback) override;
	void setProbingLearnedClauseCallback(const ProbingLearnedClauseCallback& callback) override;

	// Get the number of variables of the formula
	int getVariablesCount() override;

	int getNumOriginalDiversifications() override;
	
	// Get a variable suitable for search splitting
	int getSplittingVariable() override;

	// Get solver statistics
	void writeStatistics(SolverStatistics& stats) override;

	bool supportsIncrementalSat() override {return true;}
	bool exportsConditionalClauses() override {return false;}

	void cleanUp() override;
};
