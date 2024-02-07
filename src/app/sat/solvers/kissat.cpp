
#include "app/sat/data/clause_metadata.hpp"
#include "util/random.hpp"
#include "util/sys/timer.hpp"
#include "util/tsl/robin_set.h"

#include "util/logger.hpp"
#include "util/permutation.hpp"
extern "C" {
#include "kissat/src/kissat.h"
}
#include "kissat.hpp"
#include "util/distribution.hpp"




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
	: PortfolioSolverInterface(setup), solver(kissat_init()),
        learntClauseBuffer(_setup.strictMaxLitsPerClause+ClauseMetadata::numInts()) {

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

    // Basic configuration options for all solvers
    kissat_set_option(solver, "quiet", 1);
    kissat_set_option(solver, "check", 0); // do not check model or derived clauses
    
    // Set random seed
    kissat_set_option(solver, "seed", seed);

    // Eliminated variables obstruct the import of many shared clauses (40-90%!).
    // They are caused by BVE ("eliminate") and equivalent literal substitution.
    if (_setup.eliminationSetting == SolverSetup::DISABLE_ALL) {
        kissat_set_option(solver, "eliminate", 0);
        kissat_set_option(solver, "substitute", 0);
    }
    // Since these are important inprocessing techniques, we may want to cycle through all combinations
    // of enabling/disabling them.
    if (_setup.eliminationSetting == SolverSetup::DISABLE_MOST) {
        if (getDiversificationIndex() % 2 >= 1)
            kissat_set_option(solver, "eliminate", 0);
        if (getDiversificationIndex() % 4 >= 2)
            kissat_set_option(solver, "substitute", 0);
    }
    if (_setup.eliminationSetting == SolverSetup::DISABLE_SOME && getDiversificationIndex() % 2 == 1) {
        // Every second configuration, a subset of elim/sub is disabled.
        int divIdx = (getDiversificationIndex() / 2) % 3;
        if (divIdx % 2 == 0)
            kissat_set_option(solver, "eliminate", 0);
        if (divIdx % 4 < 2)
            kissat_set_option(solver, "substitute", 0);
    }

    if (_setup.diversifyNative) {
        if (_setup.flavour == PortfolioSequence::SAT) {
            switch (getDiversificationIndex() % getNumOriginalDiversifications()) {
                case 0: /*use default*/ break;
                case 1: kissat_set_configuration(solver, "sat"); break;
                case 2: kissat_set_configuration(solver, "plain"); break;
                case 3: kissat_set_option(solver, "eliminate", 0); break;
            }
        } else {
            if (_setup.flavour != PortfolioSequence::DEFAULT) {
                LOGGER(_logger, V1_WARN, "[WARN] Unsupported flavor - overriding with default\n");
                _setup.flavour = PortfolioSequence::DEFAULT;
            }
            // Base portfolio of different configurations
            switch (getDiversificationIndex() % getNumOriginalDiversifications()) {
                case 0: kissat_set_option(solver, "eliminate", 0); break;
                case 1: kissat_set_option(solver, "restartint", 10); break;
                case 2: kissat_set_option(solver, "walkinitially", 1); break;
                case 3: kissat_set_option(solver, "restartint", 100); break;
                case 4: kissat_set_option(solver, "sweep", 0); break;
                case 5: kissat_set_configuration(solver, "unsat"); break;
                case 6: kissat_set_configuration(solver, "sat"); break;
                case 7: kissat_set_option(solver, "probe", 0); break;
                case 8: kissat_set_option(solver, "minimizedepth", 1e4); break;
                case 9: kissat_set_option(solver, "reducefraction", 90); break;
                case 10: kissat_set_option(solver, "vivifyeffort", 1000); break;
            }
        }
    }

    // Randomize ("jitter") certain options around their default value
    if (getDiversificationIndex() >= getNumOriginalDiversifications() && _setup.diversifyNoise) {
        std::mt19937 rng(seed);
        Distribution distribution(rng);

        // Randomize restart frequency
        double meanRestarts = kissat_get_option(solver, "restartint");
        double maxRestarts = std::min(10e4, 20*meanRestarts);
        distribution.configure(Distribution::NORMAL, std::vector<double>{
            /*mean=*/meanRestarts, /*stddev=*/10, /*min=*/1, /*max=*/maxRestarts
        });
        int restartFrequency = (int) std::round(distribution.sample());
        kissat_set_option(solver, "restartint", restartFrequency);

        // Randomize score decay
        double meanDecay = kissat_get_option(solver, "decay");
        distribution.configure(Distribution::NORMAL, std::vector<double>{
            /*mean=*/meanDecay, /*stddev=*/3, /*min=*/1, /*max=*/200
        });
        int decay = (int) std::round(distribution.sample());
        kissat_set_option(solver, "decay", decay);
        
        LOGGER(_logger, V3_VERB, "Sampled restartint=%i decay=%i\n", restartFrequency, decay);
    }

    seedSet = true;
    setClauseSharing(getNumOriginalDiversifications());

    interruptionInitialized = true;
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

void Kissat::cleanUp() {
    if (solver) {
        setSolverInterrupt();
        kissat_release(solver);
        solver = nullptr;
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

void Kissat::consumeClause(int** clause, int* size, int* lbd) {
    Mallob::Clause c;
    bool success = fetchLearnedClause(c, GenericClauseStore::ANY);
    if (success) {
        assert(c.begin != nullptr);
        assert(c.size >= 1);
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

Kissat::~Kissat() {
	cleanUp();
}

