
#include <assert.h>
#include <bits/std_abs.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <cmath>
#include <functional>
#include <random>

#include "app/sat/data/clause_metadata.hpp"
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

bool begin_formula_report(void* state, int vars, int cls) {
    return ((Kissat*) state)->isPreprocessingAcceptable(vars, cls);
}

void report_preprocessed_lit(void* state, int lit) {
    ((Kissat*) state)->addLiteralFromPreprocessing(lit);
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
    kissat_set_option(solver, "quiet", 1); // do not log to stdout / stderr
    kissat_set_option(solver, "check", 0); // do not check model or derived clauses
    kissat_set_option(solver, "factor", 0); // do not perform bounded variable addition

    kissat_set_option(solver, "profile", _setup.profilingLevel);

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

    if (_setup.solverType == 'v') {
        configureBoundedVariableAddition();
        seedSet = true;
        interruptionInitialized = true;
        return;
    }

    if (_setup.solverType == 'p') {
        LOGGER(_logger, V3_VERB, "Formula before preprocessing: %i vars, %i clauses\n",
            _setup.numVars, _setup.numOriginalClauses);
            kissat_set_preprocessing_report_callback(solver, this,
            begin_formula_report, report_preprocessed_lit);
        kissat_set_option(solver, "factor", 1); // do perform bounded variable addition
        //kissat_set_option(solver, "luckyearly", 0); // lucky before preprocess can take very long
        seedSet = true;
        interruptionInitialized = true;
        return;
    }

    bool ok = true;
    if (_setup.flavour == PortfolioSequence::SAT) {
        switch (getDiversificationIndex() % 4) {
            case 0: ok = kissat_set_configuration(solver, "sat"); break;
            case 1: /*use default*/ break;
            case 2:
                ok = kissat_set_option(solver, "preprocess", 0); assert(ok);
                ok = kissat_set_option(solver, "simplify", 0); assert(ok);
                break;
            case 3: kissat_set_option(solver, "eliminate", 0); break;
        }
    } else if (_setup.flavour == PortfolioSequence::PLAIN) {
        LOGGER(_logger, V4_VVER, "plain\n");
        bool partitionedVivification = false;
        ok = kissat_set_option(solver, "lucky", 0); assert(ok);
        ok = kissat_set_option(solver, "preprocess", 0); assert(ok);
        ok = kissat_set_option(solver, "simplify", partitionedVivification); assert(ok);
        ok = kissat_set_option(solver, "probe", 0);
        if (partitionedVivification) {
            // need to keep simplify (and probe) enabled for vivification
            // -- disable everything else tied to simplify (and probe)
            ok = kissat_set_option(solver, "congruence", 0); assert(ok);
            ok = kissat_set_option(solver, "substitute", 0); assert(ok);
            ok = kissat_set_option(solver, "backbone", 0); assert(ok);
            ok = kissat_set_option(solver, "eliminate", 0); assert(ok);
            ok = kissat_set_option(solver, "sweep", 0); assert(ok);
            ok = kissat_set_option(solver, "transitive", 0); assert(ok);
            //kissat_set_option(solver, "groupindex", _setup.globalId);
            //kissat_set_option(solver, "groupsize", _setup.maxNumSolvers);
        }

        if (_setup.plainAddSpecific==1) { //Add just sweep to some solvers, but didnt work, need to understand how preprocessing techniques are intertangled
            if (getDiversificationIndex()%20==0) {
                ok = kissat_set_option(solver, "sweep", 1); assert(ok);
                //Since sweep activates probe, and probe actives further default options, we need to disable them explicitly
                ok = kissat_set_option(solver, "congruence", 0); assert(ok);
                ok = kissat_set_option(solver, "substitute", 0); assert(ok);
                ok = kissat_set_option(solver, "backbone", 0); assert(ok);
                ok = kissat_set_option(solver, "eliminate", 0); assert(ok);
                ok = kissat_set_option(solver, "transitive", 0); assert(ok);
                //ok = kissat_set_option(solver, "factor", 0); assert(ok); assertion failed...
                ok = kissat_set_option(solver, "vivify", 0); assert(ok);
            }
        }

    } else {
        if (_setup.flavour != PortfolioSequence::DEFAULT) {
            LOGGER(_logger, V1_WARN, "[WARN] Unsupported flavor - overriding with default\n");
            _setup.flavour = PortfolioSequence::DEFAULT;
        }
        if (_setup.diversifyNative) {
            // Base portfolio of different configurations
            switch (getDiversificationIndex() % getNumOriginalDiversifications()) {
                case 0: kissat_set_option(solver, "eliminate", 0); break;
                case 1: kissat_set_option(solver, "restartint", 10); break;
                case 2: kissat_set_option(solver, "walkinitially", 1); break;
                case 3: kissat_set_option(solver, "restartint", 100); break;
                case 4: kissat_set_option(solver, "sweep", 0); break;
                case 5: ok = kissat_set_configuration(solver, "unsat"); break;
                case 6: ok = kissat_set_configuration(solver, "sat"); break;
                case 7: kissat_set_option(solver, "probe", 0); break;
                case 8: kissat_set_option(solver, "minimizedepth", 1e4); break;
                case 9: kissat_set_option(solver, "reducefraction", 90); break;
                case 10: kissat_set_option(solver, "vivifyeffort", 1000); break;
            }
        }
    }
    assert(ok);

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
        int decay = kissat_get_option(solver, "decay");
        if(_setup.decayDistribution==1) { //Gaussian
            //double meanDecay = kissat_get_option(solver, "decay");
            distribution.configure(Distribution::NORMAL, std::vector<double>{
                /*mean=*/(double)_setup.decayMean, /*stddev=*/(double)_setup.decayStddev, /*min=*/(double)_setup.decayMin, /*max=*/(double)_setup.decayMax
            });
            decay = (int) std::round(distribution.sample());
            kissat_set_option(solver, "decay", decay);
        }
        else if(_setup.decayDistribution==2) { //Normal
            distribution.configure(Distribution::UNIFORM, std::vector<double>{
                /*min=*/(double)_setup.decayMin, /*max=*/(double)_setup.decayMax
            });
            decay = (int) std::round(distribution.sample());
            kissat_set_option(solver, "decay", decay);
        }

        LOGGER(_logger, V3_VERB, "--\n");
        LOGGER(_logger, V3_VERB, "Decay Sampling Distribution type=%i\n", _setup.decayDistribution);
        LOGGER(_logger, V3_VERB, "mean=%i stddev=%i min=%i max=%i \n", _setup.decayMean, _setup.decayStddev, _setup.decayMin, _setup.decayMax);
        LOGGER(_logger, V3_VERB, "Sampled restartint=%i decay=%i\n", restartFrequency, decay);
    }


    //Randomize reduce bounds
    if (getDiversificationIndex() >= getNumOriginalDiversifications() && (_setup.diversifyReduce > 0)) {
        std::mt19937 rng(seed);
        Distribution distribution(rng);

        //Give each Kissat solver a randomized reduce-range, such that some solvers keep most clauses and other solvers other kick most clauses
        int reduce_low;
        int reduce_high;
        LOGGER(_logger, V3_VERB, "--\n");

        //Reduce==1: Uniform distribution of range values
        if(_setup.diversifyReduce==1) {
            distribution.configure(Distribution::UNIFORM, std::vector<double>{
                    /*min=*/(double)(_setup.reduceMin - _setup.reduceDelta), /*max=*/(double) (_setup.reduceMax + _setup.reduceMax)
            });
            int reduce_center = (int) std::round(distribution.sample());
            reduce_low  = std::max(0, reduce_center - _setup.reduceDelta);
            reduce_high = std::min(1000, reduce_center + _setup.reduceDelta);
            LOGGER(_logger, V3_VERB, "Given diversifyReduce=%i, reduceMin=%i, reduceMax=%i, reduceDelta=%i\n", _setup.diversifyReduce, _setup.reduceMin, _setup.reduceMax, _setup.reduceDelta);
            LOGGER(_logger, V3_VERB, "Sampled reduce_center=%i\n", reduce_center);
        }

        //Reduce==2: More extreme range values, 80% of solvers are either full-keep or full-kick, only 20% of solvers are moderate with keep half
        else if(_setup.diversifyReduce==2) {
            distribution.configure(Distribution::UNIFORM, std::vector<double>{0,1});
            double random_selector = distribution.sample();
            if      (random_selector<0.4) reduce_low = reduce_high = 0;
            else if (random_selector<0.6) reduce_low = reduce_high = 500;
            else                          reduce_low = reduce_high = 1000;
            LOGGER(_logger, V3_VERB, "Given diversifyReduce=%i, reduceDelta=%i\n", _setup.diversifyReduce, _setup.reduceDelta);
        }
        //Reduce==3: Gaussian Distribution
        else if(_setup.diversifyReduce==3) {
            distribution.configure(Distribution::NORMAL, std::vector<double>{
                /*mean=*/(double)_setup.reduceMean, /*stddev=*/(double)_setup.reduceStddev, /*min=*/(double)_setup.reduceMin, /*max=*/(double)_setup.reduceMax
            });
            int reduce_fixed = (int) std::round(distribution.sample());
            reduce_low  = reduce_fixed;
            reduce_high = reduce_fixed;
            LOGGER(_logger, V3_VERB, "Given diversifyReduce=%i, reduceMin=%i, reduceMax=%i, reduceMean=%i, reduceStddev=%i \n",
                _setup.diversifyReduce, _setup.reduceMin, _setup.reduceMax, _setup.reduceMean, _setup.reduceStddev);
        }
        else {
            cout << "Error: div-reduce="<<_setup.diversifyReduce<<" is not a valid flag" << endl;
        }
        kissat_set_option(solver, "reducelow", reduce_low);
        kissat_set_option(solver, "reducehigh", reduce_high);
        LOGGER(_logger, V3_VERB, "Sampled reducelow    =%i\n", reduce_low);
        LOGGER(_logger, V3_VERB, "Sampled reducehigh   =%i\n", reduce_high);
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

void Kissat::configureBoundedVariableAddition() {
    kissat_set_option(solver, "probe", 1);
    kissat_set_option(solver, "preprocess", 1);
    kissat_set_option(solver, "preprocessrounds", 1'000'000);
    kissat_set_option(solver, "preprocessbackbone", 0);
    kissat_set_option(solver, "preprocesscongruence", 0);
    kissat_set_option(solver, "preprocessfactor", 1);
    kissat_set_option(solver, "preprocessprobe", 1);
    kissat_set_option(solver, "preprocessrounds", 0);
    kissat_set_option(solver, "preprocessweep", 0);
    kissat_set_option(solver, "factor", 1);
    kissat_set_option(solver, "factoreffort", 1'000'000);
    kissat_set_option(solver, "factoriniticks", 1'000'000);
    kissat_set_option(solver, "factorexport", 1);
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
