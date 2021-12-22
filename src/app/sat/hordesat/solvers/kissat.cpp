
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
    glueLimit = _setup.softInitialMaxLbd;
}

void Kissat::addLiteral(int lit) {
	kissat_add(solver, lit);
    numVars = std::max(numVars, std::abs(lit));
}

void Kissat::diversify(int seed) {

    if (seedSet) return;

	// Options may only be set in the initialization phase, so the seed cannot be re-set
    _logger.log(V3_VERB, "Diversifying %i\n", getDiversificationIndex());

    kissat_set_option(solver, "seed", seed);
    kissat_set_option(solver, "quiet", 1);
    kissat_set_option(solver, "check", 0); // do not check model or derived clauses

    switch (getDiversificationIndex() % getNumOriginalDiversifications()) {
    case 0: kissat_set_configuration(solver, "default"); break;
    case 1: kissat_set_configuration(solver, "sat"); break;
    case 2: kissat_set_configuration(solver, "unsat"); break;
    case 3: kissat_set_configuration(solver, "plain"); break;
    }

    seedSet = true;
}

int Kissat::getNumOriginalDiversifications() {
	return 4;
}

void Kissat::setPhase(const int var, const bool phase) {
	// TODO not implemented yet.
}

// Solve the formula with a given set of assumptions
// return 10 for SAT, 20 for UNSAT, 0 for UNKNOWN
SatResult Kissat::solve(size_t numAssumptions, const int* assumptions) {

	// TODO handle assumptions?
    assert(numAssumptions == 0);

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
            log_return_false("[ERROR] value of variable %i/%i returned %i\n", 
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
    kissat_set_clause_export_callback(solver, this, learntClauseBuffer, _setup.softMaxClauseLength, &produce_clause);
    kissat_set_clause_import_callback(solver, this, &consume_clause);
}

void Kissat::produceClause(int size, int lbd) {
    Clause c;
    if (size > _setup.hardMaxClauseLength || lbd > _setup.hardFinalMaxLbd) 
        return;
    c.size = size;
    c.lbd = lbd;
    c.begin = learntClauseBuffer;
    callback(c, _setup.localId);
}

void Kissat::consumeClause(int** clause, int* size, int* lbd) {
    Clause c;
    // Only import unit clauses for now
    bool success = fetchLearnedClause(c, ImportBuffer::UNITS_ONLY);
    if (success) {
        assert(c.begin != nullptr);
        assert(c.size == 1);
        producedUnit = *c.begin;
        *clause = &producedUnit;
        *size = 1;
        *lbd = 1;
    } else {
        *clause = 0;
        *size = 0;
    }
}

void Kissat::increaseClauseProduction() {
	glueLimit = std::min(glueLimit+1, _setup.softFinalMaxLbd);
}

int Kissat::getVariablesCount() {
	return numVars;
}

int Kissat::getSplittingVariable() {
	// TODO ?
    return 0;
}

void Kissat::writeStatistics(SolvingStatistics& stats) {
    kissat_statistics kstats = kissat_get_statistics(solver);
    stats.conflicts = kstats.conflicts;
    stats.decisions = kstats.decisions;
    stats.propagations = kstats.propagations;
    stats.restarts = kstats.restarts;
}

Kissat::~Kissat() {
	kissat_release(solver);
}

