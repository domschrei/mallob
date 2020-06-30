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

// void cbConsumeUnits(void* sp, int** start, int** end) {
// 	Lingeling* lp = (Lingeling*)sp;

// 	if (lp->unitsToAdd.empty() || (lp->clauseAddMutex.tryLock() == false)) {
// 		*start = lp->unitsBuffer;
// 		*end = lp->unitsBuffer;
// 		return;
// 	}
// 	if (lp->unitsToAdd.size() >= lp->unitsBufferSize) {
// 		lp->unitsBufferSize = 2*lp->unitsToAdd.size();
// 		lp->unitsBuffer = (int*)realloc((void*)lp->unitsBuffer, lp->unitsBufferSize * sizeof(int));
// 	}

// 	for (size_t i = 0; i < lp->unitsToAdd.size(); i++) {
// 		lp->unitsBuffer[i] = lp->unitsToAdd[i];
// 	}

// 	*start = lp->unitsBuffer;
// 	*end = *start + lp->unitsToAdd.size();
// 	lp->clauseAddMutex.unlock();
// }

// void cbConsumeCls(void* sp, int** clause, int* glue) {
// 	Lingeling* lp = (Lingeling*)sp;

// 	if (lp->learnedClausesToAdd.empty()) {
// 		*clause = NULL;
// 		return;
// 	}
// 	if (lp->clauseAddMutex.tryLock() == false) {
// 		*clause = NULL;
// 		return;
// 	}
// 	vector<int> cls = lp->learnedClausesToAdd.back();
// 	lp->learnedClausesToAdd.pop_back();

// 	if (cls.size()+1 >= lp->clsBufferSize) {
// 		lp->clsBufferSize = 2*cls.size();
// 		lp->clsBuffer = (int*)realloc((void*)lp->clsBuffer, lp->clsBufferSize * sizeof(int));
// 	}
// 	// to avoid zeros in the array, 1 was added to the glue
// 	*glue = cls[0]-1;
// 	for (size_t i = 1; i < cls.size(); i++) {
// 		lp->clsBuffer[i-1] = cls[i];
// 	}
// 	lp->clsBuffer[cls.size()-1] = 0;
// 	*clause = lp->clsBuffer;
// 	lp->clauseAddMutex.unlock();
// }

Cadical::Cadical(LoggingInterface& logger, int globalId, int localId, std::string jobname, bool addOldDiversifications)
	: PortfolioSolverInterface(logger, globalId, localId, jobname), solver(new CaDiCaL::Solver), terminator(logger), learner(*this)  {
	
	solver->connect_terminator(&terminator);

	unitsBufferSize = clsBufferSize = 100;
	unitsBuffer = (int*) malloc(unitsBufferSize*sizeof(int));
	clsBuffer = (int*) malloc(clsBufferSize*sizeof(int));

	if (addOldDiversifications) {
		numDiversifications = 20;
	} else {
		numDiversifications = 14;
	}
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

	// add the clauses
	clauseAddMutex.lock();
	for (auto clauseToAdd : clausesToAdd) {
		for (auto litToAdd : clauseToAdd) {
			addLiteral(litToAdd);
		}
		addLiteral(0);
	}
	clausesToAdd.clear();
	clauseAddMutex.unlock();

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
	//Not yet implemented
}

void Cadical::setLearnedClauseCallback(LearnedClauseCallback* callback) {
	learner.setCallback(callback);
	solver->connect_learner(&learner);
	// lglsetconsumeunits(solver, cbConsumeUnits, this);
	// lglsetconsumecls(solver, cbConsumeCls, this);
}

void Cadical::increaseClauseProduction() {
	learner.incGlueLimit();
}

int Cadical::getVariablesCount() {
	return solver->vars();
}

int Cadical::getNumOriginalDiversifications() {
	return numDiversifications;
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
	free(unitsBuffer);
	free(clsBuffer);
}
