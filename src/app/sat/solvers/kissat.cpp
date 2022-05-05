
extern "C" {
#include "kissat/src/kissat.h"
}
#include "kissat.hpp"




void produce_clause(void* state, int size, int glue) {
    ((Kissat*) state)->produceClause(size, glue);
}

void consume_clause(void* state, int** clause, int* size, int* glue) {
    ((Kissat*) state)->consumeClause(clause, size, glue);
}

int terminate_callback(void* state) {
    return ((Kissat*) state)->shouldTerminate() ? 1 : 0;
}




Kissat::Kissat(const SolverSetup& setup)
	: PortfolioSolverInterface(setup), solver(kissat_init()) {

    kissat_set_terminate(solver, this, &terminate_callback);
    glueLimit = _setup.strictLbdLimit;
}

void Kissat::addLiteral(int lit) {
	kissat_add(solver, lit);
    numVars = std::max(numVars, std::abs(lit));
}

void Kissat::diversify(int seed) {

    if (seedSet) return;

	// Options may only be set in the initialization phase, so the seed cannot be re-set
    LOGGER(_logger, V3_VERB, "Diversifying %i\n", getDiversificationIndex());

    kissat_set_option(solver, "seed", seed);
    kissat_set_option(solver, "quiet", 1);
    kissat_set_option(solver, "check", 0); // do not check model or derived clauses

    switch (getDiversificationIndex() % getNumOriginalDiversifications()) {
    case 0: kissat_set_option(solver, "eliminate", 0); break;
    case 1: kissat_set_option(solver, "delay", 10); break;
    case 2: kissat_set_option(solver, "restartint", 100); break;
    case 3: kissat_set_option(solver, "walkinitially", 1); break;
    case 4: kissat_set_option(solver, "restartint", 1000); break;
    case 5: kissat_set_option(solver, "sweep", 0); break;
    case 6: kissat_set_configuration(solver, "unsat"); break;
    case 7: kissat_set_configuration(solver, "sat"); break;
    case 8: kissat_set_option(solver, "probe", 0); break;
    case 9: kissat_set_option(solver, "tier1", 3); kissat_set_option(solver, "tier2", 8); break;

    // unused
    case 10: kissat_set_option(solver, "walkinitially", 1); kissat_set_option(solver, "walkeffort", 1e6); break;
    case 11: kissat_set_option(solver, "tier2", 8); break;
    case 12: kissat_set_configuration(solver, "plain"); break;
    case 13: kissat_set_option(solver, "tier2", 3); break;
    case 14: kissat_set_configuration(solver, "default"); break;
    }

    seedSet = true;
    setClauseSharing(getNumOriginalDiversifications());
}

int Kissat::getNumOriginalDiversifications() {
	return 10;
}

void Kissat::setPhase(const int var, const bool phase) {
    assert(!initialVariablePhasesLocked);
	if (var >= initialVariablePhases.size())
        initialVariablePhases.resize(var+1);
    initialVariablePhases[var] = phase ? 1 : -1;
}

// Solve the formula with a given set of assumptions
// return 10 for SAT, 20 for UNSAT, 0 for UNKNOWN
SatResult Kissat::solve(size_t numAssumptions, const int* assumptions) {

	// TODO handle assumptions?
    assert(numAssumptions == 0);

    // Push the initial variable phases to kissat
    initialVariablePhasesLocked = true;
    kissat_set_initial_variable_phases (solver, initialVariablePhases.data(), initialVariablePhases.size());

	// start solving
	int res = kissat_solve(solver);
	switch (res) {
	case 10:
		return SAT;
	case 20:
		return UNSAT;
    default:
		return UNKNOWN;
	}
}

void Kissat::setSolverInterrupt() {
	interrupted = true;
}

void Kissat::unsetSolverInterrupt() {
	interrupted = false;
}

void Kissat::setSolverSuspend() {
    suspended = true;
}

void Kissat::unsetSolverSuspend() {
    suspended = false;
    suspendCondVar.notify();
}

bool Kissat::shouldTerminate() {
    while (suspended) {
        auto lock = suspendMutex.getLock();
        suspendCondVar.waitWithLockedMutex(lock, [this]() {return !suspended;});
    }
    return interrupted;
}

std::vector<int> Kissat::getSolution() {
	std::vector<int> result = {0};

	for (int i = 1; i <= getVariablesCount(); i++) {
        int val = kissat_value(solver, i);
		assert(val == i || val == -i || val == 0 || 
            LOG_RETURN_FALSE("[ERROR] value of variable %i/%i returned %i\n", 
            i, getVariablesCount(), val));
        result.push_back(val == 0 ? -i : val);
    }

	return result;
}

std::set<int> Kissat::getFailedAssumptions() {
	// TODO ?
    return std::set<int>();
}

void Kissat::setLearnedClauseCallback(const LearnedClauseCallback& callback) {
	this->callback = callback;
    kissat_set_clause_export_callback(solver, this, learntClauseBuffer, _setup.strictClauseLengthLimit, &produce_clause);
    kissat_set_clause_import_callback(solver, this, &consume_clause);
}

void Kissat::produceClause(int size, int lbd) {
    if (size > _setup.strictClauseLengthLimit) return;
    learntClause.size = size;
    // In Kissat, LBD scores are represented from 1 to len-1. => Increment LBD.
    learntClause.lbd = learntClause.size == 1 ? 1 : lbd+1; 
    if (learntClause.lbd > _setup.strictLbdLimit) return;
    learntClause.begin = learntClauseBuffer;
    callback(learntClause, _setup.localId);
}

void Kissat::consumeClause(int** clause, int* size, int* lbd) {
    Clause c;
    bool success = fetchLearnedClause(c, AdaptiveClauseDatabase::ANY);
    if (success) {
        assert(c.begin != nullptr);
        assert(c.size >= 1);
        producedClause.resize(c.size);
        memcpy(producedClause.data(), c.begin, c.size*sizeof(int));
        *clause = producedClause.data();
        *size = c.size;
        // In Kissat, LBD scores are represented from 1 to len-1. => Decrement LBD.
        *lbd = c.size == 1 ? c.lbd : c.lbd-1;
    } else {
        *clause = 0;
        *size = 0;
    }
}

int Kissat::getVariablesCount() {
	return numVars;
}

int Kissat::getSplittingVariable() {
	// TODO ?
    return 0;
}

void Kissat::writeStatistics(SolverStatistics& stats) {
    kissat_statistics kstats = kissat_get_statistics(solver);
    stats.conflicts = kstats.conflicts;
    stats.decisions = kstats.decisions;
    stats.propagations = kstats.propagations;
    stats.restarts = kstats.restarts;
    stats.imported = kstats.imported;
    stats.discarded = kstats.discarded;
    LOGGER(_logger, V4_VVER, "KISSAT_DISC_REASONS r_ee:%ld,r_ed:%ld,r_pb:%ld,r_ss:%ld,r_sw:%ld,r_tr:%ld,r_fx:%ld,r_ia:%ld,r_tl:%ld\n",
        kstats.r_ee, kstats.r_ed, kstats.r_pb, kstats.r_ss, kstats.r_sw, kstats.r_tr, kstats.r_fx, kstats.r_ia, kstats.r_tl);
}

Kissat::~Kissat() {
	kissat_release(solver);
}

