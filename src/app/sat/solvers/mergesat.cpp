// Copyright (c) 2015 Tomas Balyo, Karlsruhe Institute of Technology
// Copyright (c) 2021 Norbert Manthey
/*
 * MiniSat.cpp
 *
 *  Created on: Oct 10, 2014
 *      Author: balyo
 */

#include "mergesat.hpp"
#include "mergesat/minisat/utils/System.h"
#include "mergesat/minisat/simp/SimpSolver.h"

using namespace Minisat; // MergeSat as default still uses Minisat as namespace for compatibility


// Macros for minisat literal representation conversion
#define MINI_LIT(lit) lit > 0 ? mkLit(lit-1, false) : mkLit((-lit)-1, true)
#define INT_LIT(lit) sign(lit) ? -(var(lit)+1) : (var(lit)+1)
#define MAKE_MINI_VEC(vec, miniVec) for(size_t i=0; i<vec.size(); i++)	miniVec.push(MINI_LIT(vec[i]))


MergeSatBackend::MergeSatBackend(const SolverSetup& setup) : PortfolioSolverInterface(setup) {
	solver = new MERGESAT_NSPACE::SimpSolver();
	sizeLimit = setup.softMaxClauseLength;
	lbdLimit = setup.softInitialMaxLbd;
	callback = NULL;
	// solver->verbosity = 2;
}

MergeSatBackend::~MergeSatBackend() {
	delete solver;
}

//Get the number of variables of the formula
int MergeSatBackend::getVariablesCount() {
	return solver->nVars();
}

// Get a variable suitable for search splitting
int MergeSatBackend::getSplittingVariable() {
	return solver->lastDecision + 1;
}


// Set initial phase for a given variable
void MergeSatBackend::setPhase(const int var, const bool phase) {
	solver->setPolarity(var-1, !phase);
}

// Interrupt the SAT solving, so it can be started again with new assumptions
void MergeSatBackend::setSolverInterrupt() {
	solver->interrupt();
}
// Interrupt the SAT solving, so it can be started again with new assumptions
void MergeSatBackend::setSolverSuspend() {
	// TODO implement
}
// Interrupt the SAT solving, so it can be started again with new assumptions
void MergeSatBackend::unsetSolverSuspend() {
	// TODO implement
}

// Diversify the solver
void MergeSatBackend::diversify(int seed) {
	solver->random_seed = seed;
	solver->diversify(
		getDiversificationIndex() % getNumOriginalDiversifications(), 
		getNumOriginalDiversifications()
	);
}

int MergeSatBackend::getNumOriginalDiversifications() {
	return 32;
}

void MergeSatBackend::unsetSolverInterrupt() {
	solver->clearInterrupt();
}

/* add clauses to the solver */
void MergeSatBackend::addInternalClausesToSolver(bool firstTime) {
	vec<Lit> mcls;
	
	// Permanent clauses
	for (const auto& cls : clausesToAdd) {
		mcls.clear();
		for (int lit : cls) {
			if (firstTime) {
				int var = abs(lit);
				while (solver->nVars() < var) {
					solver->newVar();
				}
			}
			mcls.push(MINI_LIT(lit));
		}
		if (!solver->addClause(mcls)) {
			LOGGER(_logger, V4_VVER, "[MS] unsat when adding cls\n");
			break;
		}
	}
	if (clausesToAdd.size() > 0) LOGGER(_logger, V4_VVER, "[MS] received %lu clauses\n", clausesToAdd.size());
	clausesToAdd.clear();

	// Redundant learned clauses
	for (const auto& cls : learnedClausesToAdd) {
		mcls.clear();
		// skip the first int containing the glue
		for (size_t i = 1; i < cls.size(); i++) mcls.push(MINI_LIT(cls[i]));
		solver->addLearnedClause(mcls);
	}
	if (learnedClausesToAdd.size() > 0) LOGGER(_logger, V4_VVER, "[MS] received %lu learned clauses\n", learnedClausesToAdd.size());
	learnedClausesToAdd.clear();
}

// Solve the formula with a given set of assumptions
// return 10 for SAT, 20 for UNSAT, 0 for UNKNOWN
SatResult MergeSatBackend::solve(size_t numAssumptions, const int* assumptions) {

	clauseAddingLock.lock();
	addInternalClausesToSolver(/*firstTime=*/true);
	clauseAddingLock.unlock();

	vec<Lit> miniAssumptions;
	for(size_t i = 0; i < numAssumptions; i++) {
		miniAssumptions.push(MINI_LIT(*(assumptions+i)));
	}
	lbool res = solver->solveLimited(miniAssumptions);
	if (res == l_True) {
		return SAT;
	}
	if (res == l_False) {
		return UNSAT;
	}
	return UNKNOWN;
}

void MergeSatBackend::addLiteral(int lit) {
	auto lock = clauseAddingLock.getLock();
	if (lit == 0) {
		clausesToAdd.push_back(clauseToAdd);
		clauseToAdd.clear();
	} else {
		clauseToAdd.push_back(lit);
	}
}

void MergeSatBackend::addLearnedClause(const Mallob::Clause& c) {
	auto lock = clauseAddingLock.getLock();
	(c.size == 1 ? clausesToAdd : learnedClausesToAdd).push_back(std::vector<int>(c.begin, c.begin+c.size));
	// this will be picked up by the solver without restarting it
}

void miniLearnCallback(const std::vector<int>& cls, int glueValue, void* issuer) {
	MergeSatBackend* mp = (MergeSatBackend*)issuer;

	if (cls.size() > mp->sizeLimit) return;
	if (glueValue > mp->lbdLimit) return;
	if (cls.size() == 0) return;
	 
	std::vector<int> ncls(cls);
	mp->callback(Mallob::Clause{ncls.data(), (int)ncls.size(), glueValue}, mp->getLocalId());
}

void consumeSharedCls(void* issuer) {
	MergeSatBackend* mp = (MergeSatBackend*)issuer;

	if (mp->learnedClausesToAdd.empty()) {
		return;
	}
	if (mp->clauseAddingLock.tryLock() == false) {
		return;
	}

	/* add clauses to the current solver */
	mp->addInternalClausesToSolver(/*firstTime=*/false);

	mp->clauseAddingLock.unlock();
}

void MergeSatBackend::setLearnedClauseCallback(const LearnedClauseCallback& callback) {
	this->callback = callback;
	solver->learnedClsCallback = miniLearnCallback;
	solver->consumeSharedCls = consumeSharedCls;
	solver->issuer = this;
}

SolverStatistics MergeSatBackend::getStatistics() {
	SolverStatistics st;
	st.conflicts = solver->conflicts;
	st.propagations = solver->propagations;
	st.restarts = solver->starts;
	st.decisions = solver->decisions;
	st.memPeak = memUsedPeak();
	return st;
}

std::vector<int> MergeSatBackend::getSolution() {
	std::vector<int> result;
	result.push_back(0);
	for (int i = 1; i <= solver->nVars(); i++) {
		auto val = solver->modelValue(MINI_LIT(i));
		result.push_back(val == l_True ? i : -i);
	}
	return result;
}

std::set<int> MergeSatBackend::getFailedAssumptions() {
	return std::set<int>(); // TODO Implement
}
