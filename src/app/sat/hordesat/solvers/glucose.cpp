
#include <ctype.h>
#include <stdarg.h>
#include <chrono>

#include "glucose.hpp"
#include "app/sat/hordesat/utilities/debug_utils.hpp"

MGlucose::MGlucose(LoggingInterface& logger, int globalId, int localId, std::string jobname) 
		: SimpSolver(), PortfolioSolverInterface(logger, globalId, localId, jobname) {
	
	lastTermCallbackTime = logger.getTime();

	stopSolver = 0;
	learnedClauseCallback = NULL;

	glueLimit = 2;

	unitsBufferSize = clsBufferSize = 100;
	unitsBuffer = (int*) malloc(unitsBufferSize*sizeof(int));
	clsBuffer = (int*) malloc(clsBufferSize*sizeof(int));

    suspendSolver = false;
    maxvar = 0;

	numDiversifications = 1; // TODO
}

void MGlucose::addLiteral(int lit) {
	if (abs(lit) > maxvar) maxvar = abs(lit);
	// TODO
}

void MGlucose::diversify(int rank, int size) {
	random_seed = rank;
	// TODO
}

// Set initial phase for a given variable
void MGlucose::setPhase(const int var, const bool phase) {
	// TODO setPolarity():  phase ? var : -var
}

// Solve the formula with a given set of assumptions
// return 10 for SAT, 20 for UNSAT, 0 for UNKNOWN
SatResult MGlucose::solve(const vector<int>& assumptions) {
	
	this->assumptions = assumptions;
	// add the clauses
	clauseAddMutex.lock();
	for (size_t i = 0; i < clausesToAdd.size(); i++) {
		for (size_t j = 0; j < clausesToAdd[i].size(); j++) {
			int lit = clausesToAdd[i][j];
			if (abs(lit) > maxvar) maxvar = abs(lit);
			// TODO add lit
		}
		// TODO end clause
	}
	clausesToAdd.clear();
	clauseAddMutex.unlock();

	// set the assumptions
	for (size_t i = 0; i < assumptions.size(); i++) {
		// freezing problems
		int lit = assumptions[i];
		if (abs(lit) > maxvar) maxvar = abs(lit);
		// TODO assume
	}
	lbool res = solveLimited(assumptions);
	// TODO return values
}

void MGlucose::setSolverInterrupt() {
	stopSolver = 1;
}
void MGlucose::unsetSolverInterrupt() {
	stopSolver = 0;
}
void MGlucose::setSolverSuspend() {
    suspendSolver = true;
}
void MGlucose::unsetSolverSuspend() {
    suspendSolver = false;
	suspendCond.notify();
}

vector<int> MGlucose::getSolution() {
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

set<int> MGlucose::getFailedAssumptions() {
	set<int> result;
	for (size_t i = 0; i < assumptions.size(); i++) {
		if (lglfailed(solver, assumptions[i])) {
			result.insert(assumptions[i]);
		}
	}
	return result;
}

void MGlucose::addLearnedClause(const int* begin, int size) {
	auto lock = clauseAddMutex.getLock();
	if (size == 1) {
		unitsToAdd.push_back(*begin);
	} else {
		learnedClausesToAdd.emplace_back(begin, begin+size);
	}
}

void MGlucose::setLearnedClauseCallback(LearnedClauseCallback* callback) {
	this->callback = callback;
	lglsetproducecls(solver, cbProduce, this);
	lglsetproduceunit(solver, cbProduceUnit, this);
	lglsetconsumeunits(solver, cbConsumeUnits, this);
	lglsetconsumecls(solver, cbConsumeCls, this);
}

void MGlucose::increaseClauseProduction() {
	if (glueLimit < 8) glueLimit++;
}

int MGlucose::getVariablesCount() {
	return maxvar;
}

int MGlucose::getNumOriginalDiversifications() {
	return numDiversifications;
}

// Get a variable suitable for search splitting
int MGlucose::getSplittingVariable() {
	// TODO possible?
}

SolvingStatistics MGlucose::getStatistics() {
	SolvingStatistics st;
	// TODO
	return st;
}

MGlucose::~MGlucose() {
	// TODO ?
	free(unitsBuffer);
	free(clsBuffer);
}
