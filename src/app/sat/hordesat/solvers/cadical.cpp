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

//const int CLAUSE_LEARN_INTERRUPT_THRESHOLD = 10000;

Cadical::Cadical(const SolverSetup& setup)
	: PortfolioSolverInterface(setup),
	  solver(new CaDiCaL::Solver), terminator(*setup.logger), 
	  learner(_setup), learnSource(_setup, [this]() {
		  Mallob::Clause c;
		  fetchLearnedClause(c, ImportBuffer::ANY);
		  return c;
	  }) {
	
	solver->connect_terminator(&terminator);
	solver->connect_learn_source(&learnSource);
}

void Cadical::addLiteral(int lit) {
	solver->add(lit);
}

void Cadical::diversify(int seed) {

	// Options may only be set in the initialization phase, so the seed cannot be re-set
	if (!seedSet) {
		_logger.log(V3_VERB, "Diversifying %i\n", getDiversificationIndex());
		bool okay = solver->set("seed", seed);
		assert(okay);
		switch (getDiversificationIndex() % getNumOriginalDiversifications()) {
		case 1: okay = solver->configure ("plain"); break;
		case 2: okay = solver->set("walk", 0); break;
		case 3: okay = solver->set("inprocessing", 0); break;
		case 4: okay = solver->set("restartint", 100); break;
		case 5: okay = solver->set("phase", 0); break;
		case 6: okay = solver->set("decompose", 0); break;
		case 7: okay = solver->set("elim", 0); break;
		case 0: default: break;
		//case 6: solver->set("cover", 1); break;
		//case 7: solver->set("chrono", 0); break;
		}
		assert(okay);
		if (getDiversificationIndex() >= getNumOriginalDiversifications()) 
			okay = solver->set("shuffle", 1);
		assert(okay);
		seedSet = true;
	}
}

int Cadical::getNumOriginalDiversifications() {
	return 8;
}

void Cadical::setPhase(const int var, const bool phase) {
	solver->phase(phase ? var : -var);
}

// Solve the formula with a given set of assumptions
// return 10 for SAT, 20 for UNSAT, 0 for UNKNOWN
SatResult Cadical::solve(size_t numAssumptions, const int* assumptions) {

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
	this->assumptions.clear();
	for (size_t i = 0; i < numAssumptions; i++) {
		int lit = assumptions[i];
		solver->assume(lit);
		this->assumptions.push_back(lit);
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

std::vector<int> Cadical::getSolution() {
	std::vector<int> result = {0};

	for (int i = 1; i <= getVariablesCount(); i++)
		result.push_back(solver->val(i));

	return result;
}

std::set<int> Cadical::getFailedAssumptions() {
	std::set<int> result;
	for (auto assumption : assumptions)
		if (solver->failed(assumption))
			result.insert(assumption);

	return result;
}

void Cadical::setLearnedClauseCallback(const LearnedClauseCallback& callback) {
	learner.setCallback(callback);
	solver->connect_learner(&learner);
}

void Cadical::increaseClauseProduction() {
	learner.incGlueLimit();
}

int Cadical::getVariablesCount() {
	return solver->vars();
}

int Cadical::getSplittingVariable() {
	return solver->lookahead();
}

void Cadical::writeStatistics(SolvingStatistics& stats) {
	CaDiCaL::Solver::Statistics s = solver->get_stats();
	stats.conflicts = s.conflicts;
	stats.decisions = s.decisions;
	stats.propagations = s.propagations;
	stats.restarts = s.restarts;
}

Cadical::~Cadical() {
	solver.release();
}
