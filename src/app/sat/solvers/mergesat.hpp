// Copyright (c) 2015 Tomas Balyo, Karlsruhe Institute of Technology
// Copyright (c) 2021 Norbert Manthey
/*
 * MergeSat.h
 *
 *  Created on: Oct 9, 2014
 *      Author: balyo
 */

#pragma once

#include "portfolio_solver_interface.hpp"
#include "util/sys/threading.hpp"
#include "util/logger.hpp"

#define CLS_COUNT_INTERRUPT_LIMIT 300

// allow to modify the name of the namespace, if required
#ifndef MERGESAT_NSPACE
#define MERGESAT_NSPACE Minisat
#endif

// some forward declatarations for MergeSat
namespace MERGESAT_NSPACE {
	class SimpSolver;
	class Lit;
	template<class T> class vec;
}

class MergeSatBackend : public PortfolioSolverInterface {

private:
	MERGESAT_NSPACE::SimpSolver *solver;
	
	std::vector<std::vector<int>> learnedClausesToAdd;
	std::vector<std::vector<int>> clausesToAdd;
	std::vector<int> clauseToAdd;

	Mutex clauseAddingLock;
	LearnedClauseCallback callback;
	int sizeLimit;
	int lbdLimit;

	friend void miniLearnCallback(const std::vector<int>& cls, int glueValue, void* issuer);
	friend void consumeSharedCls(void* issuer);

public:
	MergeSatBackend(const SolverSetup& setup);
	~MergeSatBackend() override;

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

	// Add a learned clause to the formula
	// The learned clauses might be added later or possibly never
	void addLearnedClause(const Mallob::Clause& clause) override;

	// Set a function that should be called for each learned clause
	void setLearnedClauseCallback(const LearnedClauseCallback& callback) override;

	// Request the solver to produce more clauses
	void increaseClauseProduction() override;
	
	// Get the number of variables of the formula
	int getVariablesCount() override;

	int getNumOriginalDiversifications() override;
	
	// Get a variable suitable for search splitting
	int getSplittingVariable() override;

	// Get solver statistics
	SolverStatistics getStatistics() override;

	bool supportsIncrementalSat() override {return false;}
	bool exportsConditionalClauses() override {return true;}

	void addInternalClausesToSolver(bool firstTime);
};
