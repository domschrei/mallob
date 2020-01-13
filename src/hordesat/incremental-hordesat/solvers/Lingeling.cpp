/*
 * Lingeling.cpp
 *
 *  Created on: Nov 11, 2014
 *      Author: balyo
 */

#include "Lingeling.h"
#include <ctype.h>
#include "../utilities/DebugUtils.h"
#include "../utilities/Logger.h"

extern "C" {
	#include "lglib.h"
}

int termCallback(void* solverPtr) {
	Lingeling* lp = (Lingeling*)solverPtr;

	double elapsed = getTime() - lp->lastTermCallbackTime;
	lp->lastTermCallbackTime = getTime();
    
    if (lp->suspendSolver) {
        // Stay inside this function call as long as solver is suspended
        lp->suspendMutex.lock();
        while (lp->suspendSolver) {
            pthread_cond_wait(lp->suspendCond.get(), lp->suspendMutex.mutex());
        }
        lp->suspendMutex.unlock();
    }
    
	if (lp->stopSolver) {
		log(0, "STOPPING solver (%.4fs since last term callback)", elapsed);
	}
    return lp->stopSolver;
}

void produceUnit(void* sp, int lit) {
	vector<int> vcls;
	vcls.push_back(lit);
	Lingeling* lp = (Lingeling*)sp;
	lp->callback->processClause(vcls, lp->myId);
}

void produce(void* sp, int* cls, int glue) {
	// unit clause, call produceUnit
	if (cls[1] == 0) {
		produceUnit(sp, cls[0]);
		return;
	}
	Lingeling* lp = (Lingeling*)sp;
	if (glue > lp->glueLimit) {
		return;
	}
	vector<int> vcls;
	// to avoid zeros in the array, 1 is added to the glue
	vcls.push_back(1+glue);
	int i = 0;
	while (cls[i] != 0) {
		vcls.push_back(cls[i]);
		i++;
	}
	//printf("glue = %d, size = %lu\n", glue, vcls.size());
	lp->callback->processClause(vcls, lp->myId);
}

void consumeUnits(void* sp, int** start, int** end) {
	Lingeling* lp = (Lingeling*)sp;

	if (lp->unitsToAdd.empty() || (lp->clauseAddMutex.tryLock() == false)) {
		*start = lp->unitsBuffer;
		*end = lp->unitsBuffer;
		return;
	}
	if (lp->unitsToAdd.size() >= lp->unitsBufferSize) {
		lp->unitsBufferSize = 2*lp->unitsToAdd.size();
		lp->unitsBuffer = (int*)realloc((void*)lp->unitsBuffer, lp->unitsBufferSize * sizeof(int));
	}

	for (size_t i = 0; i < lp->unitsToAdd.size(); i++) {
		lp->unitsBuffer[i] = lp->unitsToAdd[i];
	}

	*start = lp->unitsBuffer;
	*end = *start + lp->unitsToAdd.size();
	lp->clauseAddMutex.unlock();
}

void consumeCls(void* sp, int** clause, int* glue) {
	Lingeling* lp = (Lingeling*)sp;

	if (lp->learnedClausesToAdd.empty()) {
		*clause = NULL;
		return;
	}
	if (lp->clauseAddMutex.tryLock() == false) {
		*clause = NULL;
		return;
	}
	vector<int> cls = lp->learnedClausesToAdd.back();
	lp->learnedClausesToAdd.pop_back();

	if (cls.size()+1 >= lp->clsBufferSize) {
		lp->clsBufferSize = 2*cls.size();
		lp->clsBuffer = (int*)realloc((void*)lp->clsBuffer, lp->clsBufferSize * sizeof(int));
	}
	// to avoid zeros in the array, 1 was added to the glue
	*glue = cls[0]-1;
	for (size_t i = 1; i < cls.size(); i++) {
		lp->clsBuffer[i-1] = cls[i];
	}
	lp->clsBuffer[cls.size()-1] = 0;
	*clause = lp->clsBuffer;
	lp->clauseAddMutex.unlock();
}

Lingeling::Lingeling() {
	solver = lglinit();
	//lglsetopt(solver, "verbose", 10);
	// BCA has to be disabled for valid clause sharing (or freeze all literals)
	lglsetopt(solver, "bca", 0);
	lglsetopt(solver, "termint", -1);
	lastTermCallbackTime = getTime();

	stopSolver = 0;
	callback = NULL;
	lglseterm(solver, termCallback, this);
	glueLimit = 2;

	unitsBufferSize = clsBufferSize = 100;
	unitsBuffer = (int*) malloc(unitsBufferSize*sizeof(int));
	clsBuffer = (int*) malloc(clsBufferSize*sizeof(int));
	myId = 0;

    suspendSolver = false;
    suspendMutex = VerboseMutex("suspendLgl", NULL);
    //suspendCond = ConditionVariable();
    maxvar = 0;
}

bool Lingeling::loadFormula(const char* filename) {
	vector<PortfolioSolverInterface*> solvers;
	solvers.push_back(this);
	return loadFormulaToSolvers(solvers, filename);
}

int Lingeling::getVariablesCount() {
	return maxvar;
}

// Get a variable suitable for search splitting
int Lingeling::getSplittingVariable() {
	//TODO not sure what this is?
	return lglookahead(solver);
}

// Set initial phase for a given variable
void Lingeling::setPhase(const int var, const bool phase) {
	lglsetphase(solver, phase ? var : -var);
}

// Interrupt the SAT solving, so it can be started again with new assumptions
void Lingeling::setSolverInterrupt() {
	stopSolver = 1;
}
void Lingeling::unsetSolverInterrupt() {
	stopSolver = 0;
}
void Lingeling::setSolverSuspend() {
    suspendSolver = true;
}
void Lingeling::unsetSolverSuspend() {
    suspendSolver = false;
    pthread_cond_broadcast(suspendCond.get());
}

// Solve the formula with a given set of assumptions
// return 10 for SAT, 20 for UNSAT, 0 for UNKNOWN
SatResult Lingeling::solve(const vector<int>& assumptions) {
	this->assumptions = assumptions;
	// add the clauses
	clauseAddMutex.lock();
	for (size_t i = 0; i < clausesToAdd.size(); i++) {
		for (size_t j = 0; j < clausesToAdd[i].size(); j++) {
			int lit = clausesToAdd[i][j];
			if (abs(lit) > maxvar) maxvar = abs(lit);
			lgladd(solver, lit);
		}
		lgladd(solver, 0);
	}
	clausesToAdd.clear();
	clauseAddMutex.unlock();

	// set the assumptions
	for (size_t i = 0; i < assumptions.size(); i++) {
		// freezing problems
		int lit = assumptions[i];
		if (abs(lit) > maxvar) maxvar = abs(lit);
		lglassume(solver, lit);
	}
	int res = lglsat(solver);
	switch (res) {
	case LGL_SATISFIABLE:
		return SAT;
	case LGL_UNSATISFIABLE:
		return UNSAT;
	}
	return UNKNOWN;
}

vector<int> Lingeling::getSolution() {
	vector<int> result;
	result.push_back(0);
	for (int i = 1; i <= maxvar; i++) {
		if (lglderef(solver, i) > 0) {
			result.push_back(i);
		} else {
			result.push_back(-i);
		}
	}
	return result;
}

set<int> Lingeling::getFailedAssumptions() {
	set<int> result;
	for (size_t i = 0; i < assumptions.size(); i++) {
		if (lglfailed(solver, assumptions[i])) {
			result.insert(assumptions[i]);
		}
	}
	return result;
}


// Add a permanent clause to the formula
void Lingeling::addClause(vector<int>& clause) {
	clauseAddMutex.lock();
	clausesToAdd.push_back(clause);
	clauseAddMutex.unlock();
}

void Lingeling::addClauses(vector<vector<int> >& clauses) {
	clauseAddMutex.lock();
	clausesToAdd.insert(clausesToAdd.end(), clauses.begin(), clauses.end());
	clauseAddMutex.unlock();
}

void Lingeling::addClauses(const vector<int>& clauses) {
	clauseAddMutex.lock();
	vector<int> clause;
	for (size_t i = 0; i < clauses.size(); i++) {
		int lit = clauses[i];
		if (lit == 0) {
			clausesToAdd.push_back(clause);
			clause.clear();
		} else {
			clause.push_back(lit);	
		}
	}
	clauseAddMutex.unlock();
}

void Lingeling::addLiteral(int lit) {
	if (lit != 0) {
		lglfreeze(solver, lit);
	}
	if (abs(lit) > maxvar) maxvar = abs(lit);
	lgladd(solver, lit);
}


void Lingeling::addInitialClauses(vector<vector<int> >& clauses) {
	for (size_t i = 0; i < clauses.size(); i++) {
		for (size_t j = 0; j < clauses[i].size(); j++) {
			int lit = clauses[i][j];
			if (abs(lit) > maxvar) maxvar = abs(lit);
			lgladd(solver, lit);
		}
		lgladd(solver, 0);
	}
}

void Lingeling::addInitialClauses(const vector<int>& clauses) {
	for (size_t i = 0; i < clauses.size(); i++) {
		int lit = clauses[i];
		if (abs(lit) > maxvar) maxvar = abs(lit);
		lgladd(solver, lit);
	}
}

// Add a learned clause to the formula
void Lingeling::addLearnedClause(vector<int>& clause) {
	clauseAddMutex.lock();
	if (clause.size() == 1) {
		unitsToAdd.push_back(clause[0]);
	} else {
		learnedClausesToAdd.push_back(clause);
	}
	clauseAddMutex.unlock();
}

void Lingeling::addLearnedClauses(vector<vector<int> >& clauses) {
	clauseAddMutex.lock();
	for (size_t i = 0; i < clauses.size(); i++) {
		if (clauses[i].size() == 1) {
			unitsToAdd.push_back(clauses[i][0]);
		} else {
			learnedClausesToAdd.push_back(clauses[i]);
		}
	}
	clauseAddMutex.unlock();
}

void Lingeling::increaseClauseProduction() {
	glueLimit++;
}

void Lingeling::setLearnedClauseCallback(LearnedClauseCallback* callback, int solverId) {
	this->callback = callback;
	lglsetproducecls(solver, produce, this);
	lglsetproduceunit(solver, produceUnit, this);
	lglsetconsumeunits(solver, consumeUnits, this);
	lglsetconsumecls(solver, consumeCls, this);
	myId = solverId;
}

SolvingStatistics Lingeling::getStatistics() {
	SolvingStatistics st;
	st.conflicts = lglgetconfs(solver);
	st.decisions = lglgetdecs(solver);
	st.propagations = lglgetprops(solver);
	st.memPeak = lglmaxmb(solver);
	return st;
}

void Lingeling::diversify(int rank, int size) {
	// This method is copied from Plingeling
	lglsetopt(solver, "seed", rank);
	lglsetopt(solver, "flipping", 0);
    switch (rank % 16) {
		case 0: default: break;
		case 1: lglsetopt (solver, "plain", 1); break;
		case 2: lglsetopt (solver, "agilelim", 100); break;
		case 3: lglsetopt (solver, "block", 0), lglsetopt (solver, "cce", 0); break;
		case 4: lglsetopt (solver, "bias", -1); break;
		case 5: lglsetopt (solver, "acts", 0); break;
		case 6: lglsetopt (solver, "phase", 1); break;
		case 7: lglsetopt (solver, "acts", 1); break;
		case 8: lglsetopt (solver, "bias", 1); break;
		case 9:
			lglsetopt (solver, "wait", 0);
			lglsetopt (solver, "blkrtc", 1);
			lglsetopt (solver, "elmrtc", 1);
			break;
		case 10: lglsetopt (solver, "phase", -1); break;
		case 11: lglsetopt (solver, "prbsimplertc", 1); break;
		case 12: lglsetopt (solver, "gluescale", 1); break;
		case 13: lglsetopt (solver, "gluescale", 3); break;
		case 14: lglsetopt (solver, "move", 1); break;
		case 15: lglsetopt(solver, "flipping", 1); break;
	}
}



Lingeling::~Lingeling() {
	lglrelease(solver);
	free(unitsBuffer);
	free(clsBuffer);
}

