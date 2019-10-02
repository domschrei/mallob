/*
 * MiniSat.cpp
 *
 *  Created on: Oct 10, 2014
 *      Author: balyo
 */

#include "minisat/utils/System.h"
#include "minisat/core/Dimacs.h"
#include "../utilities/DebugUtils.h"
#include "MiniSat.h"
#include "minisat/core/Solver.h"

using namespace Minisat;


// Macros for minisat literal representation conversion
#define MINI_LIT(lit) lit > 0 ? mkLit(lit-1, false) : mkLit((-lit)-1, true)
#define INT_LIT(lit) sign(lit) ? -(var(lit)+1) : (var(lit)+1)
#define MAKE_MINI_VEC(vec, miniVec) for(size_t i=0; i<vec.size(); i++)	miniVec.push(MINI_LIT(vec[i]))


MiniSat::MiniSat() {
	solver = new Solver();
	learnedLimit = 0;
	myId = 0;
	callback = NULL;
}

MiniSat::~MiniSat() {
	delete solver;
}


bool MiniSat::loadFormula(const char* filename) {
    gzFile in = gzopen(filename, "rb");
    parse_DIMACS(in, *solver);
    gzclose(in);
    return true;
}

//Get the number of variables of the formula
int MiniSat::getVariablesCount() {
	return solver->nVars();
}

// Get a variable suitable for search splitting
int MiniSat::getSplittingVariable() {
	return solver->lastDecision + 1;
}


// Set initial phase for a given variable
void MiniSat::setPhase(const int var, const bool phase) {
	solver->setPolarity(var-1, phase ? l_True : l_False);
}

// Interrupt the SAT solving, so it can be started again with new assumptions
void MiniSat::setSolverInterrupt() {
	solver->interrupt();
}
void MiniSat::unsetSolverInterrupt() {
	solver->clearInterrupt();
}

vector<int> MiniSat::getSolution() {
	throw logic_error("not implemented");
}

set<int> MiniSat::getFailedAssumptions() {
	throw logic_error("not implemented");
}

// Solve the formula with a given set of assumptions
// return 10 for SAT, 20 for UNSAT, 0 for UNKNOWN
SatResult MiniSat::solve(const vector<int>& assumptions) {

	clauseAddingLock.lock();

	for (size_t ind = 0; ind < clausesToAdd.size(); ind++) {
		vec<Lit> mcls;
		MAKE_MINI_VEC(clausesToAdd[ind], mcls);
		if (!solver->addClause(mcls)) {
			clauseAddingLock.unlock();
			printf("unsat when adding cls\n");
			return UNSAT;
		}
	}
	clausesToAdd.clear();
	for (size_t ind = 0; ind < learnedClausesToAdd.size(); ind++) {
		vec<Lit> mlcls;
		// skipping the first int containing the glue
		for(size_t i = 1; i < learnedClausesToAdd[ind].size(); i++) {
			mlcls.push(MINI_LIT(learnedClausesToAdd[ind][i]));
		}
		solver->addLearnedClause(mlcls);
	}
	learnedClausesToAdd.clear();
	clauseAddingLock.unlock();

	vec<Lit> miniAssumptions;
	MAKE_MINI_VEC(assumptions, miniAssumptions);
	lbool res = solver->solveLimited(miniAssumptions);
	if (res == l_True) {
		return SAT;
	}
	if (res == l_False) {
		return UNSAT;
	}
	return UNKNOWN;
}

void MiniSat::addLiteral(int lit) {
	puts("Function not implemented!");
	exit(1);
}


void MiniSat::addClause(vector<int>& clause) {
	clauseAddingLock.lock();
	clausesToAdd.push_back(clause);
	clauseAddingLock.unlock();
	setSolverInterrupt();
}

void MiniSat::addLearnedClause(vector<int>& clause) {
	clauseAddingLock.lock();
	if (clause.size() == 1) {
		clausesToAdd.push_back(clause);
	} else {
		learnedClausesToAdd.push_back(clause);
	}
	clauseAddingLock.unlock();
	if (learnedClausesToAdd.size() > CLS_COUNT_INTERRUPT_LIMIT) {
		setSolverInterrupt();
	}
}

void MiniSat::addClauses(vector<vector<int> >& clauses) {
	clauseAddingLock.lock();
	clausesToAdd.insert(clausesToAdd.end(), clauses.begin(), clauses.end());
	clauseAddingLock.unlock();
	setSolverInterrupt();
}

void MiniSat::addInitialClauses(vector<vector<int> >& clauses) {
	for (size_t ind = 0; ind < clauses.size(); ind++) {
		vec<Lit> mcls;
		for (size_t i = 0; i < clauses[ind].size(); i++) {
			int lit = clauses[ind][i];
			int var = abs(lit);
			while (solver->nVars() < var) {
				solver->newVar();
			}
			mcls.push(MINI_LIT(lit));
		}
		if (!solver->addClause(mcls)) {
			printf("unsat when adding initial cls\n");
		}
	}
}

void MiniSat::addLearnedClauses(vector<vector<int> >& clauses) {
	clauseAddingLock.lock();
	for (size_t i = 0; i < clauses.size(); i++) {
		if (clauses[i].size() == 1) {
			clausesToAdd.push_back(clauses[i]);
		} else {
			learnedClausesToAdd.push_back(clauses[i]);
		}
	}
	clauseAddingLock.unlock();
	if (learnedClausesToAdd.size() > CLS_COUNT_INTERRUPT_LIMIT || clausesToAdd.size() > 0) {
		setSolverInterrupt();
	}
}

void miniLearnCallback(const vec<Lit>& cls, void* issuer) {
	MiniSat* mp = (MiniSat*)issuer;
	if (cls.size() > mp->learnedLimit) {
		return;
	}
	vector<int> ncls;
	if (cls.size() > 1) {
		// fake glue value
		int madeUpGlue = min(3, cls.size());
		ncls.push_back(madeUpGlue);
	}
	for (int i = 0; i < cls.size(); i++) {
		ncls.push_back(INT_LIT(cls[i]));
	}
	mp->callback->processClause(ncls, mp->myId);
}

void MiniSat::setLearnedClauseCallback(LearnedClauseCallback* callback, int solverId) {
	this->callback = callback;
	solver->learnedClsCallback = miniLearnCallback;
	solver->issuer = this;
	learnedLimit = 3;
	myId = solverId;
}

void MiniSat::increaseClauseProduction() {
	learnedLimit++;
}

void MiniSat::diversify(int rank, int size) {
	solver->random_seed = rank;
}

SolvingStatistics MiniSat::getStatistics() {
	SolvingStatistics st;
	st.conflicts = solver->conflicts;
	st.propagations = solver->propagations;
	st.restarts = solver->starts;
	st.decisions = solver->decisions;
	st.memPeak = memUsedPeak();
	return st;
}
