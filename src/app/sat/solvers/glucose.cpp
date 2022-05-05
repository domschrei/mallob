
#include <ctype.h>
#include <stdarg.h>
#include <chrono>
#include <random>

#include "glucose.hpp"

/*
Note that this file (specifically MGlucose::parallelImportClauses()) is non-free licensed 
as it uses adapted code from Glucose. For its licensing see the LICENSE file in src/app/sat/solvers/glucose.
*/

MGlucose::MGlucose(const SolverSetup& setup) 
		: SimpSolver(), PortfolioSolverInterface(setup) {
	
	if (supportsIncrementalSat() && setup.doIncrementalSolving) 
		setIncrementalMode();

	verbosity = -1;
	verbEveryConflicts = 0;
	parsing = 0;

	stopSolver = 0;
	learnedClauseCallback = NULL;

	suspendSolver = false;
	maxvar = 0;

	numDiversifications = 8;
}

void MGlucose::addLiteral(int lit) {
	resetMaps();
	nomodel = true;
	if (lit != 0) { 
		clause.push(encodeLit(lit));
		if (incremental) {
			while (maxvar < std::abs(lit)) {
				maxvar++;
				setFrozen(Glucose::var(encodeLit(maxvar)), true);
			}
		} else {
			maxvar = std::max(maxvar, abs(lit));
		}
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
	random_seed = (std::abs(seed) % UINT16_MAX) + 1;
	adaptStrategies = false;

	if (rank >= 3) {
		// For all but the first few solvers, randomize initial
		// activity and DFS until first conflict
		randomizeFirstDescent = true;
		rnd_init_act = true;
	}

	switch (rank % numDiversifications) {
	case 0:
		// adaptive solver with simplification
		adaptStrategies = true;
		use_simplification = true;
		break;
	case 1:
		// special case for low successive conflicts
		luby_restart = true;
        luby_restart_factor = 100;
        var_decay = 0.999;
        max_var_decay = 0.999;
		break;
	case 2:
		// special case for low decision levels
		chanseokStrategy = true;
        coLBDBound = 4;
        glureduce = true;
        firstReduceDB = 2000;
        nbclausesbeforereduce = firstReduceDB;
        curRestart = (conflicts / nbclausesbeforereduce) + 1;
        incReduceDB = 0;
		break;
	case 3:
		// Glucose 2.0 (+ blocked restarts)
		var_decay = 0.95;
		max_var_decay = 0.95;
		firstReduceDB = 4000;
		lbdQueue.growTo(100);
		sizeLBDQueue = 100;
		K = 0.7;
		incReduceDB = 500;
		break;
	case 4:
		// non-adaptive solver without simplification
		adaptStrategies = false;
		use_simplification = false;
		break;
	case 5:
		// Chanseok strategy for clause deletion
		chanseokStrategy = true;
		break;
	case 6:
		// adaptive solver without simplification
		adaptStrategies = true;
		use_simplification = false;
		break;
	case 7:
		// special case for high successive conflicts
		chanseokStrategy = true;
        glureduce = true;
        coLBDBound = 3;
        firstReduceDB = 30000;
        var_decay = 0.99;
        max_var_decay = 0.99;
        randomize_on_restarts = 1;
		break;
	
	// unused
	case 8:
		// randomize "var_decay" measure
		std::mt19937 rng = std::mt19937(rank%numDiversifications);
		std::uniform_real_distribution<float> dist = std::uniform_real_distribution<float>(0, 1);
		//firstReduceDB += -50 + 100 * dist(rng);
		var_decay = std::min(var_decay -0.02 + 0.04 * dist(rng), max_var_decay);
		break;
	}

	setClauseSharing(getNumOriginalDiversifications());
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
	nomodel = true;
	resetMaps();
	clearInterrupt();

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

void MGlucose::setLearnedClauseCallback(const LearnedClauseCallback& callback) {
	this->learnedClauseCallback = callback;
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

void MGlucose::writeStatistics(SolverStatistics& stats) {
	stats.conflicts = conflicts;
	stats.decisions = decisions;
	stats.propagations = propagations;
	stats.restarts = starts;
	stats.memPeak = 0; // TODO
	stats.producedClauses = numProduced;
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
	int lit = decodeLit(p);
	assert(lit != 0);
	assert(std::abs(lit) <= maxvar || LOG_RETURN_FALSE("Literal %i exported!\n", lit));
	numProduced++;
	learntClause.begin = &lit;
	learntClause.size = 1;
	learntClause.lbd = 1;
	learnedClauseCallback(learntClause, getLocalId());
}

void MGlucose::parallelExportClause(Glucose::Clause &c, bool fromConflictAnalysis) {
	// Empty clause? Not handled here
	if (c.size() == 0) return;

	// discard clauses coming from the "DuringSearch" method
	if (!fromConflictAnalysis) return;
	if (c.wasImported()) return;

	// The "exported" field has 2 bits so we cap it at 3 (where the clause 
	// is no longer interesting for export).
	c.setExported(std::min(3u, c.getExported() + 1));
	assert(c.getExported() >= 1);
	assert(c.getExported() <= 3);
	
	// Clause does not pass the hard quality limits? Discard.
	if (c.size() > (int)_setup.strictClauseLengthLimit || c.lbd() > _setup.strictLbdLimit) 
		return;
		
	// A clause has "very good quality" iff it satisfies the *soft* limits
	// (soft because they are not strict for exporting a clause).
	bool veryGoodQuality = c.size() <= (int)_setup.qualityClauseLengthLimit 
						&& c.lbd() <= _setup.qualityLbdLimit;

	// Accept the clause if seen for the 1st time with very good quality
	// or if seen for the second time with normal quality.
	bool accept = ((c.getExported() == 1 && veryGoodQuality) 
				|| (c.getExported() == 2 && !veryGoodQuality));
	if (!accept) return;

	// unit clause
	if (c.size() == 1) {
		parallelExportUnaryClause(c[0]);
		return;
	}
	
	// assemble clause
	std::vector<int> vcls(c.size());
	for (int j = 0; j < c.size(); j++) {
		vcls[j] = decodeLit(c[j]);
		assert(vcls[j] != 0);
		assert(std::abs(vcls[j]) <= maxvar || LOG_RETURN_FALSE("Literal %i/%i exported!\n", vcls[j], maxvar));
	}
	
	// export clause
	numProduced++;
	learntClause.begin = vcls.data();
	learntClause.size = c.size();
	learntClause.lbd = (int)c.lbd();
	learnedClauseCallback(learntClause, getLocalId());
}

/*
 * This method is an adaptation of ParallelSolver::parallelImportClauses().
 */
bool MGlucose::parallelImportClauses() {

	Mallob::Clause importedClause;
	while (fetchLearnedClause(importedClause, AdaptiveClauseDatabase::NONUNITS)) {
		assert(importedClause.size > 1);

		// Assemble Glucose-style clause
		unsigned int glue = importedClause.lbd;
		Glucose::vec<Glucose::Lit> glucClause;
		for (size_t i = 0; i < importedClause.size; i++) glucClause.push(encodeLit(importedClause.begin[i]));

		//printf("Thread %d imports clause from thread %d\n", threadNumber(), importedFromThread);
		Glucose::CRef cr = ca.alloc(glucClause, true, true);
		ca[cr].setLBD(glue);

		// 0 means a broadcasted clause (good clause), 1 means a survivor clause, broadcasted
		// 1: next time we see it in analyze, we share it (follow route / broadcast depending on the global strategy, part of an ongoing experimental stuff: a clause in one Watched will be set to exported 2 when promotted.
		// 2: A broadcasted clause (or a survivor clause) do not share it anymore
		ca[cr].setExported(2);
		
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
	}
	return false;
}

void MGlucose::parallelImportUnaryClauses() {

	for (int lit : fetchLearnedUnitClauses()) {
		Glucose::Lit l = encodeLit(lit);
		if (value(var(l)) == l_Undef) {
			uncheckedEnqueue(l);
		}
	}
}
