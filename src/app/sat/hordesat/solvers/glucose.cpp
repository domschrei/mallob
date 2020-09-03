
#include <ctype.h>
#include <stdarg.h>
#include <chrono>

#include "glucose.hpp"
#include "app/sat/hordesat/utilities/debug_utils.hpp"

/*
Note that this file (specifically MGlucose::parallelImportClauses()) is non-free licensed 
as it uses adapted code from Glucose. For its licensing see the LICENSE file in src/app/sat/hordesat/glucose.
*/

MGlucose::MGlucose(LoggingInterface& logger, int globalId, int localId, std::string jobname) 
		: SimpSolver(), PortfolioSolverInterface(logger, globalId, localId, jobname) {
	
	verbosity = -1;
	verbEveryConflicts = 100000;

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
	resetMaps();
	nomodel = true;
	if (lit != 0) { 
		clause.push(encodeLit(lit));
		maxvar = std::max(maxvar, abs(lit));
	} else {
		addClause(clause);
		clause.clear();
	}
}

void MGlucose::diversify(int rank, int size) {
	random_seed = rank;
	// TODO
}

// Set initial phase for a given variable
void MGlucose::setPhase(const int var, const bool phase) {
	setPolarity(Glucose::var(encodeLit(var)), phase);
}

// Solve the formula with a given set of assumptions
// return 10 for SAT, 20 for UNSAT, 0 for UNKNOWN
SatResult MGlucose::solve(const vector<int>& asmpt) {
	
	assumptions.clear();
	for (int lit : asmpt) assumptions.push(encodeLit(lit));

	calls++;
	resetMaps();
	clearInterrupt();

	// add the clauses
	clauseAddMutex.lock();
	for (size_t i = 0; i < clausesToAdd.size(); i++) {
		for (size_t j = 0; j < clausesToAdd[i].size(); j++) {
			int lit = clausesToAdd[i][j];
			if (abs(lit) > maxvar) maxvar = abs(lit);
			addLiteral(lit);
		}
		addLiteral(0);
	}
	clausesToAdd.clear();
	clauseAddMutex.unlock();

	Glucose::lbool res = solveLimited(assumptions);
	nomodel = (res != l_True);
	return (res == l_Undef) ? UNKNOWN : (res == l_True ? SAT : UNSAT);
}

void MGlucose::setSolverInterrupt() {
	asynch_interrupt = true;
}
void MGlucose::unsetSolverInterrupt() {
	asynch_interrupt = false;
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
		result.push_back(solvedValue(i));
	}
	return result;
}

set<int> MGlucose::getFailedAssumptions() {
	set<int> result;
	for (int i = 0; i < assumptions.size(); i++) {
		if (failed(assumptions[i])) {
			result.insert(decodeLit(assumptions[i]));
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
	this->learnedClauseCallback = callback;
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
	return 0;
}

SolvingStatistics MGlucose::getStatistics() {
	SolvingStatistics st;
	st.conflicts = conflicts;
	st.decisions = decisions;
	st.propagations = propagations;
	st.restarts = starts;
	st.memPeak = 0; // TODO
	return st;
}

MGlucose::~MGlucose() {
	resetMaps();
	free(unitsBuffer);
	free(clsBuffer);
}

Glucose::Lit MGlucose::encodeLit(int lit) {
	while (std::abs(lit) > nVars()) newVar();
	return Glucose::mkLit(Glucose::Var(std::abs(lit) - 1), (lit < 0));
}

int MGlucose::decodeLit(Glucose::Lit lit) {
	return sign(lit) ? -(var(lit)+1) : (var(lit)+1);
}

void MGlucose::resetMaps() { 
	if (fmap) { 
		delete[] fmap;
		fmap = 0; 
		szfmap = 0;
	}
}

int MGlucose::solvedValue(int lit) {
	if (nomodel) return 0;
	Glucose::lbool res = modelValue(encodeLit(lit));
	return (res == l_True) ? lit : -lit;
}

bool MGlucose::failed(Glucose::Lit lit) {
	if (!fmap) buildFailedMap();
	int tmp = var(lit);
	assert (0 <= tmp && tmp < nVars());
	return fmap[tmp] != 0;
}

void MGlucose::buildFailedMap() {
	fmap = new unsigned char[szfmap = nVars()];
	memset (fmap, 0, szfmap);
	for (int i = 0; i < conflict.size (); i++) {
		int tmp = var (conflict[i]);
		assert (0 <= tmp && tmp < szfmap);
		fmap[tmp] = 1;
	}
}

bool MGlucose::parallelJobIsFinished() {
	// Use (or abuse) this method to suspend a solver during its execution
	if (suspendSolver) {
		suspendCond.wait(suspendMutex, [this]{return !suspendSolver;});
	}
	return false;
}

void MGlucose::parallelExportUnaryClause(Glucose::Lit p) {
	vector<int> vcls;
	vcls.push_back(decodeLit(p));
	learnedClauseCallback->processClause(vcls, getLocalId());
}

void MGlucose::parallelExportClauseDuringSearch(Glucose::Clause &c) {
	
	// unit clause
	if (c.size() == 1) {
		parallelExportUnaryClause(c[0]);
		return;
	}

	// check glue
	unsigned int glue = c.lbd();
	if (glue > glueLimit) {
		return;
	}
	
	// assemble clause
	vector<int> vcls(1+c.size());
	int i = 0;
	// to avoid zeros in the array, 1 is added to the glue
	vcls[i++] = 1+glue;
	for (int j = 0; j < c.size(); j++) {
		vcls[i++] = decodeLit(c[j]);
	}
	
	// export clause
	learnedClauseCallback->processClause(vcls, getLocalId());
}

/*
 * This method is an adaptation of ParallelSolver::parallelImportClauses().
 */
bool MGlucose::parallelImportClauses() {

	if (!clauseAddMutex.tryLock()) return false;

	for (const auto& importedClause : learnedClausesToAdd) {

		if (importedClause.size() == 0) {
			clauseAddMutex.unlock();
			return true;
		}

		if (importedClause.size() == 1) {
			// Unary clause
			// This is an adaptation of ParallelSolver::parallelImportUnaryClauses().
			Glucose::Lit l = encodeLit(importedClause[0]);
			if (value(var(l)) == l_Undef) {
				uncheckedEnqueue(l);
			}
			continue;
		}

		// Assemble Glucose-style clause
		// to avoid zeros in the array, 1 was added to the glue
		unsigned int glue = importedClause[0]-1;
		Glucose::vec<Glucose::Lit> glucClause;
		for (size_t i = 1; i < importedClause.size(); i++) glucClause.push(encodeLit(importedClause[i]));

		//printf("Thread %d imports clause from thread %d\n", threadNumber(), importedFromThread);
		Glucose::CRef cr = ca.alloc(glucClause, true, true);
		ca[cr].setLBD(glue);

		// 0 means a broadcasted clause (good clause), 1 means a survivor clause, broadcasted
		// 1: next time we see it in analyze, we share it (follow route / broadcast depending on the global strategy, part of an ongoing experimental stuff: a clause in one Watched will be set to exported 2 when promotted.
		// 2: A broadcasted clause (or a survivor clause) do not share it anymore
		ca[cr].setExported(0); 
		
		//ca[cr].setImportedFrom(importedFromThread);
		if(useUnaryWatched)
			unaryWatchedClauses.push(cr);
		else 
			learnts.push(cr);
		
		if (ca[cr].size() <= 2) {//|| importedRoute == 0) { // importedRoute == 0 means a glue clause in another thread (or any very good clause)
			ca[cr].setOneWatched(false); // Warning: those clauses will never be promoted by a conflict clause (or rarely: they are propagated!)
			attachClause(cr);
		} else {
			if(useUnaryWatched) {
				attachClausePurgatory(cr);
				ca[cr].setOneWatched(true);
			} else {
				attachClause(cr);
				ca[cr].setOneWatched(false);
			}
		}
		assert(ca[cr].learnt());
	}
	learnedClausesToAdd.clear();

	clauseAddMutex.unlock();
	return false;
}

void MGlucose::parallelImportUnaryClauses() {

	if (!clauseAddMutex.tryLock()) return;
	
	for (int lit : unitsToAdd) {
		Glucose::Lit l = encodeLit(lit);
		if (value(var(l)) == l_Undef) {
			uncheckedEnqueue(l);
		}
	}
	unitsToAdd.clear();

	clauseAddMutex.unlock();
}

void MGlucose::parallelImportClauseDuringConflictAnalysis(Glucose::Clause &c, Glucose::CRef confl) {
	// TODO
}