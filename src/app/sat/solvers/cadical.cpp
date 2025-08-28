/*
 * Cadical.cpp
 *
 *  Created on: Jun 26, 2020
 *      Author: schick
 */

#include <assert.h>
#include <cstdint>
#include <stdint.h>
#include <functional>
#include <algorithm>
#include <cmath>
#include <random>

#include "app/sat/data/definitions.hpp"
#include "app/sat/proof/lrat_connector.hpp"
#include "cadical.hpp"
#include "app/sat/proof/trusted/trusted_utils.hpp"
#include "util/logger.hpp"
#include "util/distribution.hpp"
#include "app/sat/data/clause.hpp"
#include "app/sat/data/portfolio_sequence.hpp"
#include "app/sat/data/solver_statistics.hpp"
#include "app/sat/execution/solver_setup.hpp"
#include "app/sat/proof/lrat_op.hpp"
#include "app/sat/sharing/store/generic_clause_store.hpp"
#include "app/sat/solvers/cadical_clause_export.hpp"
#include "app/sat/solvers/cadical_clause_import.hpp"
#include "app/sat/solvers/cadical_terminator.hpp"
#include "app/sat/solvers/portfolio_solver_interface.hpp"

Cadical::Cadical(const SolverSetup& setup)
	: PortfolioSolverInterface(setup),
	  solver(new CaDiCaL::Solver), terminator(*setup.logger), 
	  learner(_setup), learnSource(_setup, [this]() {
		  Mallob::Clause c;
		  fetchLearnedClause(c, GenericClauseStore::ANY);
		  return c;
	  }) {

	solver->connect_terminator(&terminator);
	solver->connect_learn_source(&learnSource);

	if (setup.profilingLevel >= 0) {
		bool okay = solver->set("profile", setup.profilingLevel); assert(okay);
		okay = solver->set("realtime", 1); assert(okay);
		profileFileString = setup.profilingBaseDir + "/profile." + setup.jobname
			+ "." + std::to_string(setup.globalId);
		LOGGER(_logger, V3_VERB, "will write profiling to %s\n", profileFileString.c_str());
	}

	// In certified UNSAT mode?
	if (setup.certifiedUnsat) {

		int solverRank = setup.globalId;
		int maxNumSolvers = setup.maxNumSolvers;

		auto descriptor = _lrat ? "on-the-fly LRAT checking" : "LRAT proof production";
		LOGGER(_logger, V3_VERB, "Initializing rank=%i size=%i DI=%i #C=%ld IDskips=%i with %s\n",
			solverRank, maxNumSolvers, getDiversificationIndex(), setup.numOriginalClauses, setup.nbSkippedIdEpochs,
			descriptor);

		bool okay;
		okay = solver->set("lrat", 1); assert(okay); // enable LRAT proof logging
		okay = solver->set("lratsolverid", solverRank); assert(okay); // set this solver instance's ID
		okay = solver->set("lratsolvercount", maxNumSolvers); assert(okay); // set # solvers
		okay = solver->set("lratorigclscount", INT32_MAX); assert(okay);
		okay = solver->set("lratskippedepochs", setup.nbSkippedIdEpochs); assert(okay);

		if (_lrat) {
			okay = solver->set("signsharedcls", 1); assert(okay);
			solver->trace_proof_internally(
				[&](unsigned long id, const int* lits, int nbLits, const unsigned long* hints, int nbHints, int glue) {
					_lrat->push(LratOp(id, lits, nbLits, hints, nbHints, glue));
				},
				[&](unsigned long id, const int* lits, int nbLits, const uint8_t* sigData, int sigSize) {
					// TODO Do we encode "revision" immediately after the default signature?
					int rev = * (int*) (sigData + SIG_SIZE_BYTES);
					_lrat->push(LratOp(id, lits, nbLits, sigData, rev));
				},
				[&](const unsigned long* ids, int nbIds) {
					_lrat->push(LratOp(ids, nbIds));
				},
				[&](unsigned long id) {
					unsatConclusionId = id;
				}
			);
		} else {
			okay = solver->set("binary", 1); assert(okay); // set proof logging mode to binary format
			okay = solver->set("lratdeletelines", 0); assert(okay); // disable printing deletion lines
			proofFileString = _setup.proofDir + "/proof." + std::to_string(_setup.globalId) + ".lrat";
			okay = solver->trace_proof(proofFileString.c_str()); assert(okay);
		}
	}

	if (_optimizer) {
		solver->connect_external_propagator(_optimizer.get());
		for (auto [weight, lit] : _setup.objectiveFunction)
			solver->add_observed_var(std::abs(lit));
	}
}

void Cadical::addLiteral(int lit) {
	solver->add(lit);
}

void Cadical::diversify(int seed) {

	if (seedSet) return;

	LOGGER(_logger, V3_VERB, "Diversifying %i\n", getDiversificationIndex());
	bool okay = solver->set("seed", seed);
	assert(okay);

	seedSet = true;
	setClauseSharing(getNumOriginalDiversifications());

	// Randomize ("jitter") certain options around their default value
    if (getDiversificationIndex() >= getNumOriginalDiversifications() && _setup.diversifyNoise) {
        std::mt19937 rng(seed);
        Distribution distribution(rng);

        // Randomize restart frequency
        double meanRestarts = solver->get("restartint");
        double maxRestarts = std::min(2e9, 20*meanRestarts);
        distribution.configure(Distribution::NORMAL, std::vector<double>{
            /*mean=*/meanRestarts, /*stddev=*/10, /*min=*/1, /*max=*/maxRestarts
        });
        int restartFrequency = (int) std::round(distribution.sample());
        okay = solver->set("restartint", restartFrequency); assert(okay);

        // Randomize score decay
        double meanDecay = solver->get("scorefactor");
        distribution.configure(Distribution::NORMAL, std::vector<double>{
            /*mean=*/meanDecay, /*stddev=*/3, /*min=*/500, /*max=*/1000
        });
        int decay = (int) std::round(distribution.sample());
        okay = solver->set("scorefactor", decay); assert(okay);
        
        LOGGER(_logger, V3_VERB, "Sampled restartint=%i decay=%i\n", restartFrequency, decay);
    }

	if (getDiversificationIndex() >= getNumOriginalDiversifications() && _setup.diversifyFanOut) {
		okay = solver->set("fanout", 1); assert(okay);
	}

	if (_setup.flavour == PortfolioSequence::SAT) {
		switch (getDiversificationIndex() % 3) {
		case 0: okay = solver->configure("sat"); break;
		case 1: /*default configuration*/ break;
		case 2: okay = solver->set("inprocessing", 0); break;
		}
	} else if (_setup.flavour == PortfolioSequence::PLAIN) {
		LOGGER(_logger, V4_VVER, "plain\n");
		okay = solver->configure("plain");
	} else {
		if (_setup.flavour != PortfolioSequence::DEFAULT) {
			LOGGER(_logger, V1_WARN, "[WARN] Unsupported flavor - overriding with default\n");
			_setup.flavour = PortfolioSequence::DEFAULT;
		}
		if (_setup.diversifyNative) {
			switch (getDiversificationIndex() % getNumOriginalDiversifications()) {
			// Greedy 10-portfolio according to tests on SAT2020 instances
			case 0: okay = solver->set("phase", 0); break;
			case 1: okay = solver->configure("sat"); break;
			case 2: okay = solver->set("elim", 0); break;
			case 3: okay = solver->configure("unsat"); break;
			case 4: okay = solver->set("condition", 1); break;
			case 5: okay = solver->set("walk", 0); break;
			case 6: okay = solver->set("restartint", 100); break;
			case 7: okay = solver->set("cover", 1); break;
			case 8: okay = solver->set("shuffle", 1) && solver->set("shufflerandom", 1); break;
			case 9: okay = solver->set("inprocessing", 0); break;
			}
		}
	}
	assert(okay);
}

int Cadical::getNumOriginalDiversifications() {
	return _setup.flavour == PortfolioSequence::SAT ? 3 : 10;
}

void Cadical::setPhase(const int var, const bool phase) {
	solver->phase(phase ? var : -var);
}

// Solve the formula with a given set of assumptions
// return 10 for SAT, 20 for UNSAT, 0 for UNKNOWN
SatResult Cadical::solve(size_t numAssumptions, const int* assumptions) {

	// add the learned clauses
	learnMutex.lock();
	for (auto clauseToAdd : learnedClauses) {
		for (auto litToAdd : clauseToAdd) {
			addLiteral(litToAdd);
		}
		addLiteral(0);
	}
	learnedClauses.clear();
	learnMutex.unlock();

	// set the assumptions
	this->assumptions.clear();
	//clearConditionalLits();
	for (size_t i = 0; i < numAssumptions; i++) {
		int lit = assumptions[i];
		solver->assume(lit);
		this->assumptions.push_back(lit);
		//addConditionalLit(-lit);
	}

	// start solving
	int res = solver->solve();
	if (_setup.onTheFlyChecking)
		solver->conclude();

	// Flush solver logs
	_logger.flush();

	switch (res) {
	case 0:
		return UNKNOWN;
	case 10:
		return SAT;
	case 20:
		return UNSAT;
	default:
		return UNKNOWN;
	}
}

void Cadical::setSolverInterrupt() {
	solver->terminate(); // acknowledged faster / checked more frequently by CaDiCaL
	terminator.setInterrupt();
}

void Cadical::unsetSolverInterrupt() {
	terminator.unsetInterrupt();
	solver->unset_terminate();
}

std::vector<int> Cadical::getSolution() {
	std::vector<int> result(1+getVariablesCount());
	result[0] = 0;
	for (int i = 1; i <= getVariablesCount(); i++)
		result[i] = solver->val(i);
	return result;
}

std::set<int> Cadical::getFailedAssumptions() {
	std::set<int> result;
	for (auto assumption : assumptions)
		if (solver->failed(assumption))
			result.insert(assumption);

	return result;
}

void Cadical::setLearnedClauseCallback(const LearnedClauseCallback& callback) {
	learner.setCallback(callback);
	solver->connect_learner(&learner);
}
void Cadical::setProbingLearnedClauseCallback(const ProbingLearnedClauseCallback& callback) {
	learner.setProbingCallback(callback);
}

int Cadical::getVariablesCount() {
	return solver->vars();
}

int Cadical::getSplittingVariable() {
	return solver->lookahead();
}

void Cadical::writeStatistics(SolverStatistics& stats) {
	if (!solver) return;
	CaDiCaL::Solver::Statistics s = solver->get_stats();
	stats.conflicts = s.conflicts;
	stats.decisions = s.decisions;
	stats.propagations = s.propagations;
	stats.restarts = s.restarts;
	stats.imported = s.imported;
	stats.discarded = s.discarded;
	LOGGER(_logger, V4_VVER, "disc_reasons r_ed:%ld,r_fx:%ld,r_wit:%ld\n",
        s.r_el, s.r_fx, s.r_wit);
}

void Cadical::cleanUp() {
	// Clean up proof output pipeline *while the solver may still be running*
	if (_setup.certifiedUnsat) {
		LOGGER(_logger, V4_VVER, "Closing proof output asynchronously\n");
		solver->close_proof_asynchronously ();
	}
	if (_setup.profilingLevel > 0) {
		LOGGER(_logger, V4_VVER, "Writing profile ...\n");
		solver->profile_to_file(profileFileString.c_str());
		LOGGER(_logger, V4_VVER, "Profile written\n");
	}
}
