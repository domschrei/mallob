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

#include "lingeling.hpp"
#include "app/sat/hordesat/utilities/debug_utils.hpp"

extern "C" {
	#include "lglib.h"
}

int cbCheckTerminate(void* solverPtr) {
	Lingeling* lp = (Lingeling*)solverPtr;

	double elapsed = lp->_logger.getTime() - lp->lastTermCallbackTime;
	lp->lastTermCallbackTime = lp->_logger.getTime();
    
	if (lp->stopSolver) {
		slog(lp, 1, "STOP (%.2fs since last cb)", elapsed);
		return 1;
	}

    if (lp->suspendSolver) {
        // Stay inside this function call as long as solver is suspended
		slog(lp, 1, "SUSPEND (%.2fs since last cb)", elapsed);

		lp->suspendCond.wait(lp->suspendMutex, [&lp]{return !lp->suspendSolver;});
		slog(lp, 2, "RESUME");

		if (lp->stopSolver) {
			slog(lp, 2, "STOP after suspension", elapsed);
			return 1;
		}
    }
    
    return 0;
}

void cbProduceUnit(void* sp, int lit) {
	vector<int> vcls(1, lit);
	Lingeling* lp = (Lingeling*)sp;
	lp->callback->processClause(vcls, lp->getLocalId());
}

void cbProduce(void* sp, int* cls, int glue) {
	// unit clause, call produceUnit
	if (cls[1] == 0) {
		cbProduceUnit(sp, cls[0]);
		return;
	}
	Lingeling* lp = (Lingeling*)sp;
	// LBD score check
	if (glue > (int)lp->glueLimit) {
		return;
	}
	// size check
	unsigned int size = 0;
	unsigned int i = 0;
	while (cls[i++] != 0) size++;
	if (size > lp->sizeLimit) return;
	// build vector of literals
	vector<int> vcls(1+size);
	// to avoid zeros in the array, 1 is added to the glue
	vcls[0] = 1+glue;
	for (i = 1; i <= size; i++) {
		vcls[i] = cls[i-1];
	}
	//printf("glue = %d, size = %lu\n", glue, vcls.size());
	lp->callback->processClause(vcls, lp->getLocalId());
}

void cbConsumeUnits(void* sp, int** start, int** end) {
	Lingeling* lp = (Lingeling*)sp;
	*start = lp->unitsBuffer;
	*end = lp->unitsBuffer;

	// Unlockable?
	if (!lp->clauseAddMutex.tryLock()) {
		return;
	}
	// Nothing to import?
	if (lp->learnedUnits.empty()) {
		lp->clauseAddMutex.unlock();
		return;
	}

	// Increase buffer size as needed
	if (lp->learnedUnits.size() >= lp->unitsBufferSize) {
		lp->unitsBufferSize = 2*lp->learnedUnits.size();
		lp->unitsBuffer = (int*)realloc((void*)lp->unitsBuffer, lp->unitsBufferSize * sizeof(int));
	}

	// Copy literals from ring buffer to the lingeling buffer
	int i = 0;
	int lit;
	while (lp->learnedUnits.consume(lit)) {
		lp->unitsBuffer[i++] = lit;
		lp->numDigested++;
	}
	// Set correct bounds
	*start = lp->unitsBuffer;
	*end = *start + lp->learnedUnits.size();

	// Return lock
	lp->clauseAddMutex.unlock();
}

void cbConsumeCls(void* sp, int** clause, int* glue) {
	Lingeling* lp = (Lingeling*)sp;

	// Unlockable?
	if (!lp->clauseAddMutex.tryLock()) {
		*clause = NULL;
		return;
	}

	// Retrieve clause to import
	vector<int> cls;
	if (!lp->learnedClauses.consume(cls)) {
		// Nothing to import
		*clause = NULL;
		lp->clauseAddMutex.unlock();
		return;
	} else lp->numDigested++;

	// Increase buffer size as needed
	if (cls.size()+1 >= lp->clsBufferSize) {
		lp->clsBufferSize = 2*cls.size();
		lp->clsBuffer = (int*)realloc((void*)lp->clsBuffer, lp->clsBufferSize * sizeof(int));
	}

	// Set glue
	*glue = cls[0]-1; // to avoid zeros in the array, 1 was added to the glue
	// Write clause into buffer
	for (size_t i = 1; i < cls.size(); i++) {
		lp->clsBuffer[i-1] = cls[i];
	}
	lp->clsBuffer[cls.size()-1] = 0;
	// Make "clause" point to buffer
	*clause = lp->clsBuffer;

	// Return lock
	lp->clauseAddMutex.unlock();
}


Lingeling::Lingeling(const SolverSetup& setup) 
	: PortfolioSolverInterface(setup), learnedClauses(LGL_CLS_BUFFER_MAX_SIZE), 
		learnedUnits(LGL_UNIT_BUFFER_MAX_SIZE) {

	solver = lglinit();
	
	//lglsetopt(solver, "verbose", 1);
	//lglsetopt(solver, "termint", 10);
	
	// BCA has to be disabled for valid clause sharing (or freeze all literals)
	lglsetopt(solver, "bca", 0);
	
	lastTermCallbackTime = _logger.getTime();

	stopSolver = 0;
	callback = NULL;

	lglsetime(solver, getTime);
	lglseterm(solver, cbCheckTerminate, this);
	glueLimit = _setup.softInitialMaxLbd;
	sizeLimit = _setup.softMaxClauseLength;

	unitsBufferSize = clsBufferSize = 100;
	unitsBuffer = (int*) malloc(unitsBufferSize*sizeof(int));
	clsBuffer = (int*) malloc(clsBufferSize*sizeof(int));

    suspendSolver = false;
    maxvar = 0;

	if (_setup.useAdditionalDiversification) {
		numDiversifications = 20;
	} else {
		numDiversifications = 14;
	}
}

void Lingeling::addLiteral(int lit) {
	
	// TODO required for incremental solving?
	//if (lit != 0) lglfreeze(solver, lit);
	
	if (abs(lit) > maxvar) maxvar = abs(lit);
	lgladd(solver, lit);
}

void Lingeling::diversify(int seed) {
	
	lglsetopt(solver, "seed", seed);
	int rank = getDiversificationIndex();
	
	// This method is based on Plingeling: OLD from ayv, NEW from bcj

	lglsetopt(solver, "classify", 0); // NEW
	//lglsetopt(solver, "flipping", 0); // OLD

    switch (rank % numDiversifications) {
		
		// Default solver
		case 0: break;

		// Alternative default solver
		case 1: 
			lglsetopt (solver, "plain", 1);
			lglsetopt (solver, "decompose", 1); // NEW 
			break;

		// NEW
		case 2: lglsetopt (solver, "restartint", 1000); break;
		case 3: lglsetopt (solver, "elmresched", 7); break;
		
		// NEW: local search solver
		case 4:
			lglsetopt (solver, "plain", rank % (2*numDiversifications) >= numDiversifications);
			lglsetopt (solver, "locs", -1);
			lglsetopt (solver, "locsrtc", 1);
			lglsetopt (solver, "locswait", 0);
			lglsetopt (solver, "locsclim", (1<<24));
			break;

		case 5: lglsetopt (solver, "scincincmin", 250); break;
		case 6: 
			lglsetopt (solver, "block", 0); 
			lglsetopt (solver, "cce", 0); 
			break;
		case 7: lglsetopt (solver, "scincinc", 50); break;
		case 8: lglsetopt (solver, "phase", -1); break;
		case 9: lglsetopt (solver, "phase", 1); break;
		case 10: lglsetopt (solver, "sweeprtc", 1); break;
		case 11: lglsetopt (solver, "restartint", 100); break;
		case 12:
			lglsetopt (solver, "reduceinit", 10000);
			lglsetopt (solver, "reducefixed", 1);
			break;
		case 13: lglsetopt (solver, "restartint", 4); break;

		// OLD
		case 14: lglsetopt (solver, "agilitylim", 100); break; // NEW from "agilelim"
		//case X: lglsetopt (solver, "bias", -1); break; // option removed
		//case X: lglsetopt (solver, "bias", 1); break; // option removed
		//case X: lglsetopt (solver, "activity", 1); break; // omitting; NEW from "acts"
		case 15: lglsetopt (solver, "activity", 2); break; // NEW from "acts", 0
		case 16:
			lglsetopt (solver, "wait", 0);
			lglsetopt (solver, "blkrtc", 1);
			lglsetopt (solver, "elmrtc", 1);
			break;
		case 17: lglsetopt (solver, "prbsimplertc", 1); break;
		//case X: lglsetopt (solver, "gluescale", 1); break; // omitting
		case 18: lglsetopt (solver, "gluescale", 5); break; // from 3 (value "ld" moved)
		case 19: lglsetopt (solver, "move", 1); break;

		default: break;
	}
}

// Set initial phase for a given variable
void Lingeling::setPhase(const int var, const bool phase) {
	lglsetphase(solver, phase ? var : -var);
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

void Lingeling::addLearnedClause(const int* begin, int size) {
	//double time = _logger.getTime();
	if (!clauseAddMutex.tryLock()) {

		//time = _logger.getTime() - time;
		//if (time > 0.2f) slog(this, -1, "[0] addLearnedClause took %.2fs!\n", time);

		return;
	}
	if (size == 1) {
		if (!learnedUnits.produce(*begin)) {
			slog(this, 2, "Unit buffer full (recv=%i digs=%i)\n", numReceived, numDigested);
		} else numReceived++;
	} else {
		if (!learnedClauses.produce(std::vector<int>(begin, begin+size))) {
			slog(this, 2, "Clause buffer full (recv=%i digs=%i)\n", numReceived, numDigested);
		} else numReceived++;
	}
	clauseAddMutex.unlock();

	//time = _logger.getTime() - time;
	//if (time > 0.2f) slog(this, -1, "[1] addLearnedClause took %.2fs! (size %i)\n", time, size);
}

void Lingeling::setLearnedClauseCallback(LearnedClauseCallback* callback) {
	this->callback = callback;
	lglsetproducecls(solver, cbProduce, this);
	lglsetproduceunit(solver, cbProduceUnit, this);
	lglsetconsumeunits(solver, cbConsumeUnits, this);
	lglsetconsumecls(solver, cbConsumeCls, this);
}

void Lingeling::increaseClauseProduction() {
	if (glueLimit < _setup.softFinalMaxLbd) glueLimit++;
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

SolvingStatistics Lingeling::getStatistics() {
	SolvingStatistics st;
	st.conflicts = lglgetconfs(solver);
	st.decisions = lglgetdecs(solver);
	st.propagations = lglgetprops(solver);
	st.memPeak = lglmaxmb(solver);
	return st;
}

Lingeling::~Lingeling() {
	lglrelease(solver);
	free(unitsBuffer);
	free(clsBuffer);
}
