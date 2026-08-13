
#include <assert.h>
#include <bits/std_abs.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <cmath>
#include <functional>

#include "app/sat/data/clause_metadata.hpp"
#include "app/sat/proof/lrat_connector.hpp"
#include "app/sat/solvers/override_config.hpp"
#include "util/logger.hpp"
#include "app/sat/data/portfolio_sequence.hpp"
#include "app/sat/data/solver_statistics.hpp"
#include "app/sat/execution/solver_setup.hpp"
#include "app/sat/sharing/store/generic_clause_store.hpp"
#include "app/sat/solvers/portfolio_solver_interface.hpp"

extern "C" {
#include "kissat/src/kissat.h"
}
#include "kissat.hpp"




void produce_clause(void* state, int size, int glue) {
    ((Kissat*) state)->produceClause(size, glue);
}

void consume_clause(void* state, int** clause, int* size, int* glue, unsigned long* id, unsigned char* sig) {
    ((Kissat*) state)->consumeClause(clause, size, glue, id, sig);
}

int terminate_callback(void* state) {
    return ((Kissat*) state)->shouldTerminate() ? 1 : 0;
}

bool begin_formula_report(void* state, int vars, int cls) {
    return ((Kissat*) state)->isPreprocessingAcceptable(vars, cls);
}

void report_preprocessed_lit(void* state, int lit) {
    ((Kissat*) state)->addLiteralFromPreprocessing(lit);
}

void on_drup_derivation(void* state, const int* lits, int nbLits, int glue) {
    //((Kissat*) state)->processProofLine(LratOp(lits, nbLits, glue));
}

void on_lrup_import(void* state, unsigned long id, const int* lits, int nbLits, const uint8_t* sigData) {
    //((Kissat*) state)->processProofLine(LratOp(id, lits, nbLits, sigData));
}

void on_drup_deletion(void* state, const int* lits, int nbLits) {
    //((Kissat*) state)->processProofLine(LratOp(lits, nbLits));
}





Kissat::Kissat(const SolverSetup& setup)
	: PortfolioSolverInterface(setup), solver(kissat_init()),
        learntClauseBuffer(_setup.strictMaxLitsPerClause+ClauseMetadata::numInts()) {

    kissat_set_terminate(solver, this, &terminate_callback);
    glueLimit = _setup.strictLbdLimit;
    numVars = setup.numVars;

    if (setup.certifiedUnsat) {
        assert(_lrat); // needs to be real-time checking setup for Kissat

        int solverRank = setup.globalId;
		int maxNumSolvers = setup.maxNumSolvers;

		auto descriptor = _lrat ? "on-the-fly checking" : "proof production";
		LOGGER(_logger, V3_VERB, "Initializing rank=%i size=%i DI=%i #C=%ld IDskips=%i with %s\n",
			solverRank, maxNumSolvers, getDiversificationIndex(), setup.numOriginalClauses, setup.nbSkippedIdEpochs,
			descriptor);

        // set Kissat's internal proof tracing mode
        kissat_trace_proof_internally(solver, this, &on_drup_derivation, &on_lrup_import, &on_drup_deletion);
    }
}

void Kissat::addLiteral(int lit) {
	kissat_add(solver, lit);
    numVars = std::max(numVars, std::abs(lit));
}

void Kissat::diversify(int seed) {

    if (seedSet) return;
    LOGGER(_logger, V3_VERB, "Diversifying %i seed=%i\n", getDiversificationIndex(), seed);

    // Basic configuration options for all solvers
    kissat_set_option(solver, "quiet", 1); // do not log to stdout / stderr
    kissat_set_option(solver, "check", 0); // do not check model or derived clauses
    kissat_set_option(solver, "factor", 0); // do not perform bounded variable addition
    kissat_set_option(solver, "seed", seed); // random seed
    // profiling (if desired)
    kissat_set_option(solver, "profile", _setup.profilingLevel);

    seedSet = true;
    setClauseSharing(getNumOriginalDiversifications());

    if (_setup.flavour == PortfolioSequence::PREPROCESS || _setup.solverType == 'p') {
        LOGGER(_logger, V3_VERB, "Formula before preprocessing: %i vars, %i clauses\n",
            _setup.numVars, _setup.numOriginalClauses);
            kissat_set_preprocessing_report_callback(solver, this,
            begin_formula_report, report_preprocessed_lit);
        kissat_set_option(solver, "factor", 1); // do perform bounded variable addition
        //kissat_set_option(solver, "luckyearly", 0); // lucky before preprocess can take very long
        interruptionInitialized = true;
        return; // do not apply overrides since a preprocessor is not part of the portfolio
    }

    applyOverrides(_setup.baseSeed);
    interruptionInitialized = true;
}

void Kissat::applyOverrides(int seed) {
    for (auto& setting : _setup.overrides.getConfigurationOverrides(
				PortfolioSequence::BaseSolver(_setup.solverType), _setup.flavour,
				_setup.diversificationIndex, seed)) {
        if (setting.key == "configure") {
            const char* conf = std::get<0>(setting.val).c_str();
            if (!kissat_has_configuration(conf)) {
                LOGGER(_logger, V0_CRIT, "[ERROR] Kissat does not have configuration %s\n", conf);
                abort();
            }
            LOGGER(_logger, V4_VVER, "conf override \"%s\"\n", conf);
            kissat_set_configuration(solver, conf);
        } else {
            long long value = setting.type == Setting::ADD ? kissat_get_option(solver, setting.key.c_str())
                : 0;
            value += std::get<1>(setting.val);
            value = std::min(value, setting.max);
            value = std::max(value, setting.min);
			LOGGER(_logger, V4_VVER, "opt override \"%s=%lld\"\n", setting.key.c_str(), value);
            kissat_set_option(solver, setting.key.c_str(), value);
        }
    }
}

int Kissat::getNumOriginalDiversifications() {
    return _setup.flavour == PortfolioSequence::SAT ? 4 : 11;
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
    if (interruptionInitialized) kissat_terminate (solver);
}

void Kissat::unsetSolverInterrupt() {
	interrupted = false;
}

bool Kissat::shouldTerminate() {
    return interrupted;
}

void Kissat::cleanUp() {
    if (_setup.profilingLevel > 0) {
        auto profileFileString = _setup.profilingBaseDir + "/profile." + _setup.jobname
            + "." + std::to_string(_setup.globalId);
		LOGGER(_logger, V4_VVER, "Writing profile ...\n");
		kissat_write_profile(solver, profileFileString.c_str());
		LOGGER(_logger, V4_VVER, "Profile written\n");
	}
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

void Kissat::reconstructSolutionFromPreprocessing(std::vector<int>& model) {
    kissat_import_model(solver, model.data(), model.size());
    model.resize(_setup.numVars+1);
    for (int v = 1; v <= _setup.numVars; v++) {
        int val = kissat_value(solver, v);
        if (std::abs(val) == v) model[v] = val;
        assert(model[v] != 0);
        //assert(std::abs(model[v]) == v || LOG_RETURN_FALSE("[ERROR] value of variable %i returned %i\n", v, model[v]));
    }
}

std::set<int> Kissat::getFailedAssumptions() {
	// TODO ?
    return std::set<int>();
}

void Kissat::setLearnedClauseCallback(const LearnedClauseCallback& callback) {
	this->callback = callback;
    kissat_set_clause_export_callback(solver, this, learntClauseBuffer.data(), _setup.strictMaxLitsPerClause, &produce_clause);
    kissat_set_clause_import_callback(solver, this, &consume_clause);
}

void Kissat::produceClause(int size, int lbd) {
    interruptionInitialized = true;
    if (size > _setup.strictMaxLitsPerClause) return;
    learntClause.size = size;
    // In Kissat, long clauses of LBD 1 can be exported. => Increment LBD in this case.
    learntClause.lbd = learntClause.size == 1 ? 1 : lbd;
    if (learntClause.lbd == 1 && learntClause.size > 1) learntClause.lbd++;
    if (learntClause.lbd > _setup.strictLbdLimit) return;
    learntClause.begin = learntClauseBuffer.data();
    callback(learntClause, _setup.localId);
}

void Kissat::consumeClause(int** clause, int* size, int* lbd, unsigned long* id, unsigned char* sig) {
    Mallob::Clause c;
    bool success = fetchLearnedClause(c, GenericClauseStore::ANY);
    if (success) {
        assert(c.begin != nullptr);
        assert(c.size >= 1);
        if (ClauseMetadata::enabled()) {
            *id = ClauseMetadata::readUnsignedLong(c.begin);
            if (ClauseMetadata::numInts() > 2) {
                memcpy(sig, c.begin+2, sizeof(int) * (ClauseMetadata::numInts()-2));
            }
        }
        *size = c.size - ClauseMetadata::numInts();
        producedClause.resize(*size);
        memcpy(producedClause.data(), c.begin+ClauseMetadata::numInts(), *size*sizeof(int));
        *clause = producedClause.data();
        *lbd = c.lbd;
    } else {
        *clause = 0;
        *size = 0;
    }
}

void Kissat::processProofLine(LratOp&& op) {
    _lrat->push(std::move(op));
}

int Kissat::getVariablesCount() {
	return numVars;
}

int Kissat::getSplittingVariable() {
	// TODO ?
    return 0;
}

void Kissat::writeStatistics(SolverStatistics& stats) {
    if (!solver) return;
    kissat_statistics kstats = kissat_get_statistics(solver);
    stats.conflicts = kstats.conflicts;
    stats.decisions = kstats.decisions;
    stats.propagations = kstats.propagations;
    stats.restarts = kstats.restarts;
    stats.imported = kstats.imported;
    stats.discarded = kstats.discarded;
    LOGGER(_logger, V4_VVER, "disc_reasons r_ee:%ld,r_ed:%ld,r_pb:%ld,r_ss:%ld,r_sw:%ld,r_tr:%ld,r_fx:%ld,r_ia:%ld,r_tl:%ld\n",
        kstats.r_ee, kstats.r_ed, kstats.r_pb, kstats.r_ss, kstats.r_sw, kstats.r_tr, kstats.r_fx, kstats.r_ia, kstats.r_tl);
}

bool Kissat::isPreprocessingAcceptable(int nbVars, int nbClauses) {
    bool accept = nbVars != _setup.numVars || nbClauses != _setup.numOriginalClauses;
    if (accept) {
        nbPreprocessedVariables = nbVars;
        nbPreprocessedClausesAdvertised = nbClauses;
    } else setSolverInterrupt();
    return accept;
}

void Kissat::addLiteralFromPreprocessing(int lit) {
    preprocessedFormula.push_back(lit);
    if (lit == 0) nbPreprocessedClausesReceived++;
    if (nbPreprocessedClausesReceived == nbPreprocessedClausesAdvertised) {
        // Full preprocessed formula received
        LOGGER(_logger, V3_VERB, "Received preprocessed formula: %i vars, %i clauses\n",
            nbPreprocessedVariables, nbPreprocessedClausesReceived);
        preprocessedFormula.push_back(nbPreprocessedVariables);
        preprocessedFormula.push_back(nbPreprocessedClausesReceived);
        setPreprocessedFormula(std::move(preprocessedFormula));
        setSolverInterrupt();
    }
}

Kissat::~Kissat() {
    if (solver) {
        setSolverInterrupt();
        kissat_release(solver);
        solver = nullptr;
    }
}
