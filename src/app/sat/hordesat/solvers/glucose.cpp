
#include <ctype.h>
#include <stdarg.h>
#include <chrono>
#include <random>

#include "glucose.hpp"
#include "app/sat/hordesat/utilities/debug_utils.hpp"

/*
Note that this file (specifically MGlucose::parallelImportClauses()) is non-free licensed 
as it uses adapted code from Glucose. For its licensing see the LICENSE file in src/app/sat/hordesat/glucose.
*/

MGlucose::MGlucose(const SolverSetup& setup) 
		: SimpSolver(), PortfolioSolverInterface(setup) {
	
	verbosity = -1;
	verbEveryConflicts = 10000000;
	parsing = 0;

	stopSolver = 0;
	learnedClauseCallback = NULL;

	suspendSolver = false;
	maxvar = 0;

	numDiversifications = 10;
	softMaxLbd = _setup.softInitialMaxLbd;
	hardMaxLbd = _setup.hardInitialMaxLbd;
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

/*
 * This method uses some of the diversification from SolverConfiguration::configureSAT14().
 */
void MGlucose::diversify(int seed) {
	int rank = getDiversificationIndex();
	random_seed = seed;
	adaptStrategies = false;

	switch (rank % numDiversifications) {
	case 0:
		break; // default solver
	case 1:
		adaptStrategies = true;
		break;
	case 2:
		use_simplification = false;
		break;
	case 3: 
		var_decay = 0.94;
		max_var_decay = 0.96;
		firstReduceDB = 600;
		break;
	case 4:
		var_decay = 0.90;
		max_var_decay = 0.97;
		firstReduceDB = 500;
		break;
	case 5:
		var_decay = 0.85;
		max_var_decay = 0.93;
		firstReduceDB = 400;
		break;
	case 6:
		// Glucose 2.0 (+ blocked restarts)
		var_decay = 0.95;
		max_var_decay = 0.95;
		firstReduceDB = 4000;
		lbdQueue.growTo(100);
		sizeLBDQueue = 100;
		K = 0.7;
		incReduceDB = 500;
		break;
	case 7:
		var_decay = 0.93;
		max_var_decay = 0.96;
		firstReduceDB = 100;
		incReduceDB = 500;
		break;
	case 8:
		var_decay = 0.75;
		max_var_decay = 0.94;
		firstReduceDB = 2000;
		break;
	case 9:
		var_decay = 0.94;
		max_var_decay = 0.96;
		firstReduceDB = 800;
		break;
	}

	if (rank > numDiversifications) {
		std::mt19937 rng = std::mt19937(seed);
		std::uniform_real_distribution<float> dist = std::uniform_real_distribution<float>(0, 1);
		firstReduceDB += -50 + 100 * dist(rng);
		var_decay = std::min(var_decay -0.01 + 0.02 * dist(rng), max_var_decay);
	}
}

// Set initial phase for a given variable
void MGlucose::setPhase(const int var, const bool phase) {
	setPolarity(Glucose::var(encodeLit(var)), phase);
}

// Solve the formula with a given set of assumptions
// return 10 for SAT, 20 for UNSAT, 0 for UNKNOWN
SatResult MGlucose::solve(size_t numAssumptions, const int* assumptions) {
	
	this->assumptions.clear();
	for (size_t i = 0; i < numAssumptions; i++) {
		int lit = assumptions[i];
		this->assumptions.push(encodeLit(lit));
	}

	calls++;
	resetMaps();
	clearInterrupt();

	// add the clauses
	clauseAddMutex.lock();
	for (size_t i = 0; i < clausesToAdd.size(); i++) {
		for (size_t j = 0; j < clausesToAdd[i].size(); j++) {
			addLiteral(clausesToAdd[i][j]);
		}
		addLiteral(0);
	}
	clausesToAdd.clear();
	clauseAddMutex.unlock();

	Glucose::lbool res = solveLimited(this->assumptions);
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

std::vector<int> MGlucose::getSolution() {
	std::vector<int> result;
	result.push_back(0);
	for (int i = 1; i <= maxvar; i++) {
		result.push_back(solvedValue(i));
	}
	return result;
}

std::set<int> MGlucose::getFailedAssumptions() {
	std::set<int> result;
	for (int i = 0; i < assumptions.size(); i++) {
		if (failed(assumptions[i])) {
			result.insert(decodeLit(assumptions[i]));
		}
	}
	return result;
}

void MGlucose::addLearnedClause(const int* begin, int size) {
	if (!clauseAddMutex.tryLock()) {
		return;
	}
	numReceived++;
	if (size == 1) {
		unitsToAdd.push_back(*begin);
	} else {
		learnedClausesToAdd.emplace_back(begin, begin+size);
	}
	clauseAddMutex.unlock();
}

void MGlucose::setLearnedClauseCallback(const LearnedClauseCallback& callback) {
	this->learnedClauseCallback = callback;
}

void MGlucose::increaseClauseProduction() {
	if (hardMaxLbd < _setup.hardFinalMaxLbd) hardMaxLbd++;
	else if (softMaxLbd < _setup.softFinalMaxLbd) softMaxLbd++;
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
	st.receivedClauses = numReceived;
	st.digestedClauses = numDigested;
	st.discardedClauses = numDiscarded;
	return st;
}

MGlucose::~MGlucose() {
	resetMaps();
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
	// Use (or rather abuse) this method to suspend a solver during its execution
	if (suspendSolver) {
		suspendCond.wait(suspendMutex, [this]{return !suspendSolver;});
	}
	return false;
}

void MGlucose::parallelExportUnaryClause(Glucose::Lit p) {
	std::vector<int> vcls;
	vcls.push_back(decodeLit(p));
	learnedClauseCallback(vcls, getLocalId());
}

void MGlucose::parallelExportClause(Glucose::Clause &c, bool fromConflictAnalysis) {
	
	// unit clause
	if (c.size() == 1) {
		parallelExportUnaryClause(c[0]);
		return;
	}

	// discard clauses coming from the "DuringSearch" method
	if (!fromConflictAnalysis) return;

	// decide whether to export the clause
	bool accept = false;
	// Export good clauses which are seen for the (at least) second time now
	// Filtering by clause length and by duplicates is done elsewhere!
	c.setExported(c.getExported() + 1);
	if (!c.wasImported() && c.getExported() == 2) {
		// Lenient check: only needs to pass "hard" limits on clause length & LBD
		if (c.size() <= (int)_setup.hardMaxClauseLength && c.lbd() <= hardMaxLbd) {
			accept = true;
		}
	}
	// Export clause unconditionally if it passes the soft checks
	accept = accept || (!c.wasImported() 
			&& c.size() <= (int)_setup.softMaxClauseLength 
			&& c.lbd() <= softMaxLbd);
	if (!accept) return;
	
	// assemble clause
	std::vector<int> vcls(1+c.size());
	int i = 0;
	// to avoid zeros in the array, 1 is added to the glue
	vcls[i++] = 1+c.lbd();
	for (int j = 0; j < c.size(); j++) {
		vcls[i++] = decodeLit(c[j]);
	}
	
	// export clause
	learnedClauseCallback(vcls, getLocalId());
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
				numDigested++;
			} else numDiscarded++;
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
			if (useUnaryWatched) {
				attachClausePurgatory(cr);
				ca[cr].setOneWatched(true);
			} else {
				attachClause(cr);
				ca[cr].setOneWatched(false);
			}
		}
		assert(ca[cr].learnt());
		numDigested++;
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
