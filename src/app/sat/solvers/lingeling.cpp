/*
 * Lingeling.cpp
 *
 *  Created on: Nov 11, 2014
 *      Author: balyo
 */

#include <ctype.h>
#include <stdarg.h>
#include <chrono>
#include <string.h>
#include <cmath>

#include "lingeling.hpp"
#include "util/sys/timer.hpp"

extern "C" {
	#include "lglib.h"
}

#ifndef LGL_UNKNOWN
#define LGL_UNKNOWN 0
#define LGL_SATISFIABLE 10
#define LGL_UNSATISFIABLE 20
#endif

/***************************** CALLBACKS *****************************/

int cbCheckTerminate(void* solverPtr) {
	Lingeling* lp = (Lingeling*)solverPtr;

	double elapsed = Timer::elapsedSeconds() - lp->lastTermCallbackTime;
	lp->lastTermCallbackTime = Timer::elapsedSeconds();
    
	if (lp->stopSolver) {
		LOGGER(lp->_logger, V3_VERB, "STOP (%.2fs since last cb)", elapsed);
		return 1;
	}

    if (lp->suspendSolver) {
        // Stay inside this function call as long as solver is suspended
		LOGGER(lp->_logger, V3_VERB, "SUSPEND (%.2fs since last cb)", elapsed);

		lp->suspendCond.wait(lp->suspendMutex, [&lp]{return !lp->suspendSolver;});
		LOGGER(lp->_logger, V4_VVER, "RESUME");

		if (lp->stopSolver) {
			LOGGER(lp->_logger, V4_VVER, "STOP after suspension", elapsed);
			return 1;
		}
    }
    
    return 0;
}
void cbProduceUnit(void* sp, int lit) {
	((Lingeling*)sp)->doProduceUnit(lit);
}
void cbProduce(void* sp, int* cls, int glue) {
	((Lingeling*)sp)->doProduce(cls, glue);
}
void cbConsumeUnits(void* sp, int** start, int** end) {
	((Lingeling*)sp)->doConsumeUnits(start, end);
}
void cbConsumeCls(void* sp, int** clause, int* glue) {
	((Lingeling*)sp)->doConsume(clause, glue);
}

/************************** END OF CALLBACKS **************************/



Lingeling::Lingeling(const SolverSetup& setup) 
	: PortfolioSolverInterface(setup),
		incremental(setup.doIncrementalSolving) {

	solver = lglinit();
	
	// BCA has to be disabled for valid clause sharing (or freeze all literals)
	// TODO can you omit this in incremental mode if all literals are frozen anyway?
	lglsetopt(solver, "bca", 0); 

	// Sync (i.e., export) unit clauses more frequently
	lglsetopt(solver, "syncunint", 11111); // down from 111'111
	
	lastTermCallbackTime = Timer::elapsedSeconds();

	stopSolver = 0;
	callback = NULL;

	lglsetime(solver, getTime);
	lglseterm(solver, cbCheckTerminate, this);
	sizeLimit = _setup.strictClauseLengthLimit;
	glueLimit = _setup.strictLbdLimit;

    suspendSolver = false;
    maxvar = 0;

	numDiversifications = 11;
}

void Lingeling::addLiteral(int lit) {
	if (lit != 0) updateMaxVar(lit);
	lgladd(solver, lit);
}

void Lingeling::updateMaxVar(int lit) {
	lit = abs(lit);
	assert(lit <= 134217723); // lingeling internal literal limit
	if (!incremental) maxvar = std::max(maxvar, lit);
	else while (maxvar < lit) {
		maxvar++;
		// Freezing required for incremental solving only.
		// This loop ensures that each literal that is added
		// or assumed at some point is frozen exactly once.
		lglfreeze(solver, maxvar);
	}
}

void Lingeling::diversify(int seed) {
	
	lglsetopt(solver, "seed", seed);
	int rank = getDiversificationIndex();

	// This method is based on Plingeling (mix of ayv and bcj)

	lglsetopt(solver, "classify", 0); // NEW
	//lglsetopt(solver, "flipping", 0); // OLD

    switch (rank % numDiversifications) {
		case 0: lglsetopt (solver, "gluescale", 5); break; // from 3 (value "ld" moved)
		case 1: 
			lglsetopt (solver, "plain", 1);
			lglsetopt (solver, "decompose", 1);
			break;
		case 2:
			lglsetopt (solver, "plain", rank % (2*numDiversifications) < numDiversifications);
			lglsetopt (solver, "locs", -1);
			lglsetopt (solver, "locsrtc", 1);
			lglsetopt (solver, "locswait", 0);
			lglsetopt (solver, "locsclim", (1<<24));
			break;
		case 3: lglsetopt (solver, "restartint", 100); break;
		case 4: lglsetopt (solver, "sweeprtc", 1); break;
		case 5: lglsetopt (solver, "restartint", 1000); break;
		case 6: lglsetopt (solver, "scincinc", 50); break;
		case 7: lglsetopt (solver, "restartint", 4); break;
		case 8: lglsetopt (solver, "phase", 1); break;
		case 9: lglsetopt (solver, "phase", -1); break;
		case 10: 
			lglsetopt (solver, "block", 0); 
			lglsetopt (solver, "cce", 0); 
			break;
	}

	setClauseSharing(getNumOriginalDiversifications());
}

// Set initial phase for a given variable
void Lingeling::setPhase(const int var, const bool phase) {
	lglsetphase(solver, phase ? var : -var);
}

// Solve the formula with a given set of assumptions
// return 10 for SAT, 20 for UNSAT, 0 for UNKNOWN
SatResult Lingeling::solve(size_t numAssumptions, const int* assumptions) {
	
	// set the assumptions
	this->assumptions.clear();
	for (size_t i = 0; i < numAssumptions; i++) {
		int lit = assumptions[i];
		updateMaxVar(lit);
		lglassume(solver, lit);
		this->assumptions.push_back(lit);
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
	suspendCond.notify();
}


void Lingeling::doProduceUnit(int lit) {
	assert(lit != 0);
	numProduced++;
	producedClause.begin = &lit;
	producedClause.size = 1;
	producedClause.lbd = 1;
	callback(producedClause, getLocalId());
}

void Lingeling::doProduce(int* cls, int glue) {
	
	// unit clause
	if (cls[1] == 0) {
		doProduceUnit(cls[0]);
		return;
	}
	// In Lingeling, LBD scores are represented from 1 to len-1. => Increment LBD.
	glue++;
	// LBD score check
	if (glueLimit != 0 && glue > (int)glueLimit) {
		return;
	}
	// size check
	int size = 0;
	unsigned int i = 0;
	while (cls[i++] != 0) size++;
	assert(size > 1);
	if (size > sizeLimit) return;
	assert(glue <= size);

	// export clause
	numProduced++;
	producedClause.begin = cls;
	producedClause.size = size;
	producedClause.lbd = glue;
	callback(producedClause, getLocalId());
}

void Lingeling::doConsumeUnits(int** start, int** end) {

	// Get as many unit clauses as possible
	unitsToAdd = fetchLearnedUnitClauses();
	*start = unitsToAdd.data();
	*end = unitsToAdd.data()+unitsToAdd.size();

	for (int lit : unitsToAdd) {
		assert(std::abs(lit) <= maxvar || 
			log_return_false("ERROR: Tried to import unit clause %i (max. var: %i)!\n", lit, maxvar));
	}
}

void Lingeling::doConsume(int** clause, int* glue) {
	*clause = nullptr;

	Mallob::Clause c;
	bool success = fetchLearnedClause(c, AdaptiveClauseDatabase::NONUNITS);
	if (!success) return;

	// Assemble a zero-terminated array of all the literals
	// (and keep it as a member until this function is called for the next time)
	assert(c.size > 1);
	zeroTerminatedClause.resize(c.size+1);
	//std::string str = "consume cls : ";
	for (size_t i = 0; i < c.size; i++) {
		int lit = c.begin[i];
		//str += std::to_string(lit) + " ";
		assert(i == 0 || std::abs(lit) <= maxvar 
			|| LOG_RETURN_FALSE("ERROR: tried to import lit %i (max. var: %i)!\n", lit, maxvar));
		zeroTerminatedClause[i] = lit;
	}
	zeroTerminatedClause[c.size] = 0;

	// In Lingeling, LBD scores are represented from 1 to len-1. => Decrement LBD.
	*glue = c.lbd-1;
	*clause = zeroTerminatedClause.data();
}

void Lingeling::setLearnedClauseCallback(const LearnedClauseCallback& callback) {
	this->callback = callback;
	lglsetproducecls(solver, cbProduce, this);
	lglsetproduceunit(solver, cbProduceUnit, this);
	lglsetconsumeunits(solver, cbConsumeUnits, this);
	lglsetconsumecls(solver, cbConsumeCls, this);
}

std::vector<int> Lingeling::getSolution() {
	std::vector<int> result;
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

std::set<int> Lingeling::getFailedAssumptions() {
	std::set<int> result;
	for (size_t i = 0; i < assumptions.size(); i++) {
		if (lglfailed(solver, assumptions[i])) {
			result.insert(assumptions[i]);
		}
	}
	return result;
}

int Lingeling::getVariablesCount() {
	return maxvar;
}

int Lingeling::getNumOriginalDiversifications() {
	return numDiversifications;
}

// Get a variable suitable for search splitting
int Lingeling::getSplittingVariable() {
	return lglookahead(solver);
}

void Lingeling::writeStatistics(SolverStatistics& stats) {
	stats.conflicts = lglgetconfs(solver);
	stats.decisions = lglgetdecs(solver);
	stats.propagations = lglgetprops(solver);
	stats.memPeak = lglmaxmb(solver);
	stats.producedClauses = numProduced;
}

Lingeling::~Lingeling() {
	lglrelease(solver);
}
