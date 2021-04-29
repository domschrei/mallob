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
#include "util/sys/timer.hpp"

extern "C" {
	#include "lglib.h"
}

int cbCheckTerminate(void* solverPtr) {
	Lingeling* lp = (Lingeling*)solverPtr;

	double elapsed = Timer::elapsedSeconds() - lp->lastTermCallbackTime;
	lp->lastTermCallbackTime = Timer::elapsedSeconds();
    
	if (lp->stopSolver) {
		lp->_logger.log(V3_VERB, "STOP (%.2fs since last cb)", elapsed);
		return 1;
	}

    if (lp->suspendSolver) {
        // Stay inside this function call as long as solver is suspended
		lp->_logger.log(V3_VERB, "SUSPEND (%.2fs since last cb)", elapsed);

		lp->suspendCond.wait(lp->suspendMutex, [&lp]{return !lp->suspendSolver;});
		lp->_logger.log(V4_VVER, "RESUME");

		if (lp->stopSolver) {
			lp->_logger.log(V4_VVER, "STOP after suspension", elapsed);
			return 1;
		}
    }
    
    return 0;
}

void cbProduceUnit(void* sp, int lit) {
	std::vector<int> vcls(1, lit);
	Lingeling* lp = (Lingeling*)sp;
	lp->callback(vcls, lp->getLocalId());
}

void cbProduce(void* sp, int* cls, int glue) {
	// unit clause, call produceUnit
	if (cls[1] == 0) {
		cbProduceUnit(sp, cls[0]);
		return;
	}
	Lingeling* lp = (Lingeling*)sp;
	// LBD score check
	if (lp->glueLimit != 0 && glue > (int)lp->glueLimit) {
		return;
	}
	// size check
	unsigned int size = 0;
	unsigned int i = 0;
	while (cls[i++] != 0) size++;
	if (size > lp->sizeLimit) return;
	// build vector of literals
	std::vector<int> vcls(1+size);
	// to avoid zeros in the array, 1 is added to the glue
	vcls[0] = 1+glue;
	for (i = 1; i <= size; i++) {
		vcls[i] = cls[i-1];
	}
	//printf("glue = %d, size = %lu\n", glue, vcls.size());
	lp->callback(vcls, lp->getLocalId());
}

void cbConsumeUnits(void* sp, int** start, int** end) {
	Lingeling* lp = (Lingeling*)sp;

	bool success = lp->learnedUnits.consume(lp->learnedUnitsBuffer);
	if (!success) lp->learnedUnitsBuffer.clear();
	
	*start = lp->learnedUnitsBuffer.data();
	*end = lp->learnedUnitsBuffer.data()+lp->learnedUnitsBuffer.size();

	lp->numDigested += lp->learnedUnitsBuffer.size();
}

void cbConsumeCls(void* sp, int** clause, int* glue) {
	Lingeling* lp = (Lingeling*)sp;

	// Retrieve clauses from parallel ringbuffer as necessary
	if (lp->learnedClausesBuffer.empty()) {
		bool success = lp->learnedClauses.consume(lp->learnedClausesBuffer);
		if (!success) lp->learnedClausesBuffer.clear();
	}
	
	// Is there a clause?
	if (lp->learnedClausesBuffer.empty()) {
		*clause = nullptr;
		return;
	}

	/*
	std::string str = ""; for (auto l : lp->learnedClausesBuffer) str += " " + std::to_string(l);
	lp->_logger.log(V4_VVER, "LCB:%s\n", str.c_str());
	*/

	// Extract a clause
	size_t pos = lp->learnedClausesBuffer.size()-1;
	assert(lp->learnedClausesBuffer[pos] == 0);
	do {pos--;} while (pos > 0 && lp->learnedClausesBuffer[pos-1] != 0);
	assert(pos == 0 || pos >= 3); // glue int, >= two literals, separation zero

	// Set glue
	*glue = lp->learnedClausesBuffer[pos]-1; // to avoid zeros in the array, 1 was added to the glue
	assert(*glue > 0);
	// Write clause into buffer
	lp->learnedClause.resize(lp->learnedClausesBuffer.size()-pos-1);
	memcpy(lp->learnedClause.data(), lp->learnedClausesBuffer.data()+pos+1, lp->learnedClause.size()*sizeof(int));
	// Make "clause" point to buffer
	*clause = lp->learnedClause.data();

	lp->learnedClausesBuffer.resize(pos);
	assert(lp->learnedClausesBuffer.empty() || lp->learnedClausesBuffer[pos-1] == 0);
	lp->numDigested++;
}


Lingeling::Lingeling(const SolverSetup& setup) 
	: PortfolioSolverInterface(setup),
		incremental(setup.incremental),
		learnedClauses(4*setup.anticipatedLitsToImportPerCycle), 
		learnedUnits(2*setup.anticipatedLitsToImportPerCycle + 1) {

	_logger.log(V4_VVER, "Local buffer sizes: %ld, %ld\n", learnedClauses.getCapacity(), learnedUnits.getCapacity());

	solver = lglinit();
	
	//lglsetopt(solver, "verbose", 1);
	//lglsetopt(solver, "termint", 10);
	
	// BCA has to be disabled for valid clause sharing (or freeze all literals)
	lglsetopt(solver, "bca", 0);
	
	lastTermCallbackTime = Timer::elapsedSeconds();

	stopSolver = 0;
	callback = NULL;

	lglsetime(solver, getTime);
	lglseterm(solver, cbCheckTerminate, this);
	glueLimit = _setup.softInitialMaxLbd;
	sizeLimit = _setup.softMaxClauseLength;

    suspendSolver = false;
    maxvar = 0;

	if (_setup.useAdditionalDiversification) {
		numDiversifications = 20;
	} else {
		numDiversifications = 14;
	}
}

void Lingeling::addLiteral(int lit) {
	
	if (lit == 0) {
		lgladd(solver, 0);
		return;
	}
	updateMaxVar(lit);
	lgladd(solver, lit);
}

void Lingeling::updateMaxVar(int lit) {
	lit = abs(lit);
	assert(lit <= 134217723); // lingeling internal literal limit
	if (!incremental) maxvar = lit;
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
			lglsetopt (solver, "plain", rank % (2*numDiversifications) < numDiversifications);
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
SatResult Lingeling::solve(size_t numAssumptions, const int* assumptions) {
	
	// set the assumptions
	this->assumptions.clear();
	for (size_t i = 0; i < numAssumptions; i++) {
		// freezing problems
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

void Lingeling::addLearnedClause(const int* begin, int size) {

	if (size == 1) {
		if (!learnedUnits.produce(begin, size, /*addSeparationZero=*/false)) {
			//_logger.log(V4_VVER, "Unit buffer full (recv=%i digs=%i)\n", numReceived, numDigested);
			numDiscarded++;
		}
	} else {
		if (!learnedClauses.produce(begin, size, /*addSeparationZero=*/true)) {
			//_logger.log(V4_VVER, "Clause buffer full (recv=%i digs=%i)\n", numReceived, numDigested);
			numDiscarded++;
		}
	}
	numReceived++;

	//time = _logger.getTime() - time;
	//if (time > 0.2f) lp->_logger.log(-1, "[1] addLearnedClause took %.2fs! (size %i)\n", time, size);
}

void Lingeling::setLearnedClauseCallback(const LearnedClauseCallback& callback) {
	this->callback = callback;
	lglsetproducecls(solver, cbProduce, this);
	lglsetproduceunit(solver, cbProduceUnit, this);
	lglsetconsumeunits(solver, cbConsumeUnits, this);
	lglsetconsumecls(solver, cbConsumeCls, this);
}

void Lingeling::increaseClauseProduction() {
	if (glueLimit != 0 && glueLimit < _setup.softFinalMaxLbd) 
		glueLimit++;
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
	st.receivedClauses = numReceived;
	st.digestedClauses = numDigested;
	st.discardedClauses = numDiscarded;
	return st;
}

Lingeling::~Lingeling() {
	lglrelease(solver);
}
