/*
 * Lingeling.cpp
 *
 *  Created on: Nov 11, 2014
 *      Author: balyo
 */

#include <ctype.h>
#include <stdarg.h>
#include <chrono>

#include "Lingeling.h"
#include "../utilities/DebugUtils.h"

extern "C" {
	#include "lglib.h"
}

using namespace std::chrono;

Mutex timeCallbackLock;
std::map<std::string, high_resolution_clock::time_point> times;
std::string currentSolverName = "";
high_resolution_clock::time_point lglSolverStartTime;

void updateTimer(std::string solverName) {
	auto lock = timeCallbackLock.getLock();
	if (currentSolverName == solverName) return;
	if (!times.count(solverName)) {
		times[solverName] = high_resolution_clock::now();
	}
	lglSolverStartTime = times[solverName];
	currentSolverName = solverName;
}
double getTime() {
    high_resolution_clock::time_point nowTime = high_resolution_clock::now();
	timeCallbackLock.lock();
    duration<double, std::milli> time_span = nowTime - lglSolverStartTime;    
	timeCallbackLock.unlock();
	return time_span.count() / 1000;
}

void slog(Lingeling* lgl, int verbosityLevel, const char* fmt, ...) {
	std::string msg = lgl->_global_name + " ";
	msg += fmt;
	va_list vl;
	va_start(vl, fmt);
	lgl->logger.log_va_list(verbosityLevel, msg.c_str(), vl);
	va_end(vl);
}

int termCallback(void* solverPtr) {
	Lingeling* lp = (Lingeling*)solverPtr;

	double elapsed = lp->logger.getTime() - lp->lastTermCallbackTime;
	lp->lastTermCallbackTime = lp->logger.getTime();
    
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

Lingeling::Lingeling(LoggingInterface& logger, int solverId, std::string jobname, 
		bool addOldDiversifications) : logger(logger), myId(solverId), jobname(jobname) {
	solver = lglinit();
	//lglsetopt(solver, "verbose", 1);
	// BCA has to be disabled for valid clause sharing (or freeze all literals)
	lglsetopt(solver, "bca", 0);
	lglsetopt(solver, "termint", 10);
	lastTermCallbackTime = logger.getTime();

	updateTimer(jobname);

	stopSolver = 0;
	callback = NULL;

	lglsetime(solver, getTime);
	lglseterm(solver, termCallback, this);
	glueLimit = 2;

	unitsBufferSize = clsBufferSize = 100;
	unitsBuffer = (int*) malloc(unitsBufferSize*sizeof(int));
	clsBuffer = (int*) malloc(clsBufferSize*sizeof(int));

    suspendSolver = false;
    //suspendMutex = VerboseMutex("suspendLgl", NULL);
    //suspendCond = ConditionVariable();
    maxvar = 0;

	if (addOldDiversifications) {
		numDiversifications = 20;
	} else {
		numDiversifications = 14;
	}
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
	updateTimer(jobname);
	stopSolver = 0;
}
void Lingeling::setSolverSuspend() {
    suspendSolver = true;
}
void Lingeling::unsetSolverSuspend() {
	updateTimer(jobname);
    suspendSolver = false;
	suspendCond.notify();
}

// Solve the formula with a given set of assumptions
// return 10 for SAT, 20 for UNSAT, 0 for UNKNOWN
SatResult Lingeling::solve(const vector<int>& assumptions) {
	updateTimer(jobname);

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
	auto lock = clauseAddMutex.getLock();
	clausesToAdd.push_back(clause);
}

void Lingeling::addClauses(vector<vector<int> >& clauses) {
	auto lock = clauseAddMutex.getLock();
	clausesToAdd.insert(clausesToAdd.end(), clauses.begin(), clauses.end());
}

void Lingeling::addClauses(const vector<int>& clauses) {
	auto lock = clauseAddMutex.getLock();
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
	auto lock = clauseAddMutex.getLock();
	if (clause.size() == 1) {
		unitsToAdd.push_back(clause[0]);
	} else {
		learnedClausesToAdd.push_back(clause);
	}
}

void Lingeling::addLearnedClauses(vector<vector<int> >& clauses) {
	auto lock = clauseAddMutex.getLock();
	for (size_t i = 0; i < clauses.size(); i++) {
		if (clauses[i].size() == 1) {
			unitsToAdd.push_back(clauses[i][0]);
		} else {
			learnedClausesToAdd.push_back(clauses[i]);
		}
	}
}

void Lingeling::increaseClauseProduction() {
	if (glueLimit < 8) glueLimit++;
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

int Lingeling::getNumOriginalDiversifications() {
	return numDiversifications;
}

void Lingeling::diversify(int rank, int size) {
	
	lglsetopt(solver, "seed", rank);
	
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


/*
case  1: setopt (i, lgl, "plain", 1),
		setopt (i, lgl, "decompose", 1); break;
case  2: setopt (i, lgl, "restartint", 1000); break;
case  3: setopt (i, lgl, "elmresched", 7); break;
case  4: setopt (i, lgl, "scincincmin", 250); break;
case  5: setopt (i, lgl, "block", 0),
		setopt (i, lgl, "cce", 0); break;
case  6: setopt (i, lgl, "scincinc", 50); break;
case  7: setopt (i, lgl, "phase", -1); break;
case  8: setopt (i, lgl, "phase", 1); break;
case  9: setopt (i, lgl, "sweeprtc", 1); break;
case 10: setopt (i, lgl, "restartint", 100); break;
case 11: setopt (i, lgl, "reduceinit", 10000);
		setopt (i, lgl, "reducefixed", 1); break;
case 12: setopt (i, lgl, "restartint", 4); break;
*/



Lingeling::~Lingeling() {
	lglrelease(solver);
	free(unitsBuffer);
	free(clsBuffer);
}

