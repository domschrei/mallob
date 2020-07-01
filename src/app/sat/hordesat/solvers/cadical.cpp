/*
 * Cadical.cpp
 *
 *  Created on: Jun 26, 2020
 *      Author: schick
 */

#include <ctype.h>
#include <stdarg.h>
#include <chrono>

#include "app/sat/hordesat/solvers/cadical.hpp"
#include "app/sat/hordesat/utilities/debug_utils.hpp"

Cadical::Cadical(LoggingInterface& logger, int globalId, int localId, std::string jobname)
	: PortfolioSolverInterface(logger, globalId, localId, jobname), solver(new CaDiCaL::Solver), terminator(logger), learner(*this) {
	
	solver->connect_terminator(&terminator);
	}

void Cadical::addLiteral(int lit) {
	solver->add(lit);
}

void Cadical::diversify(int rank, int size) {
	solver->set("seed", rank);
	// TODO: More diversification
}

void Cadical::setPhase(const int var, const bool phase) {
	solver->phase(phase ? var : -var);
}

// Solve the formula with a given set of assumptions
// return 10 for SAT, 20 for UNSAT, 0 for UNKNOWN
SatResult Cadical::solve(const vector<int>& assumptions) {
	
	// remember assumptions
	this->assumptions = assumptions;

	// add the learned clauses
	learnMutex.lock();
	for (auto clauseToAdd : learnedClauses) {
		for (auto litToAdd : clauseToAdd) {
			addLiteral(litToAdd);
		}
		addLiteral(0);
	}
	learnedClauses.clear();
	learnMutex.unlock();

	// set the assumptions
	for (auto assumption : assumptions) {
		solver->assume(assumption);
	}

	// start solving
	int res = solver->solve();
	switch (res) {
	case 0:
		return UNKNOWN;
	case 10:
		return SAT;
	case 20:
		return UNSAT;
	default:
		return UNKNOWN;
	}
}

void Cadical::setSolverInterrupt() {
	terminator.setInterrupt();
}

void Cadical::unsetSolverInterrupt() {
	terminator.unsetInterrupt();
}

void Cadical::setSolverSuspend() {
    terminator.setSuspend();
}

void Cadical::unsetSolverSuspend() {
    terminator.unsetSuspend();
}

vector<int> Cadical::getSolution() {
	vector<int> result = {0};

	for (int i = 1; i <= getVariablesCount(); i++)
		result.push_back(solver->val(i));

	return result;
}

set<int> Cadical::getFailedAssumptions() {
	set<int> result;
	for (auto assumption : assumptions)
		if (solver->failed(assumption))
			result.insert(assumption);

	return result;
}

void Cadical::addLearnedClause(const int* begin, int size) {
	auto lock = learnMutex.getLock();
	if (size == 1) {
		learnedClauses.emplace_back(begin, begin + 1);
	} else {
		// Skip glue in front of array
		learnedClauses.emplace_back(begin + 1, begin + (size - 1));
	}
}

void Cadical::setLearnedClauseCallback(LearnedClauseCallback* callback) {
	learner.setCallback(callback);
	solver->connect_learner(&learner);
}

void Cadical::increaseClauseProduction() {
	learner.incGlueLimit();
}

int Cadical::getVariablesCount() {
	return solver->vars();
}

int Cadical::getNumOriginalDiversifications() {
	return 0;
}

int Cadical::getSplittingVariable() {
	return solver->lookahead();
}

SolvingStatistics Cadical::getStatistics() {
	SolvingStatistics st;
	// Stats are currently not accessible for the outside
	// The can be directly printed with
	// solver->statistics();
	return st;
}

Cadical::~Cadical() {
	solver.release();
}
