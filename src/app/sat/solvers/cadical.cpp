/*
 * Cadical.cpp
 *
 *  Created on: Jun 26, 2020
 *      Author: schick
 */

#include <ctype.h>
#include <functional>
#include <stdarg.h>
#include <chrono>
#include <filesystem>

#include "app/sat/data/clause_metadata.hpp"
#include "app/sat/proof/trusted_solving.hpp"
#include "cadical.hpp"
#include "util/hmac_sha256/hmac_sha256.hpp"
#include "util/logger.hpp"
#include "util/distribution.hpp"

Cadical::Cadical(const SolverSetup& setup)
	: PortfolioSolverInterface(setup),
	  solver(new CaDiCaL::Solver), terminator(*setup.logger), 
	  learner(_setup), learnSource(_setup, [this]() {
		  Mallob::Clause c;
		  fetchLearnedClause(c, GenericClauseStore::ANY);
		  return c;
	  }), _do_trusted_solving(_setup.onTheFlyChecking) {

	solver->connect_terminator(&terminator);
	solver->connect_learn_source(&learnSource);

	// In certified UNSAT mode?
	if (setup.certifiedUnsat || _do_trusted_solving) {

		int solverRank = setup.globalId;
		int maxNumSolvers = setup.maxNumSolvers;

		auto descriptor = _do_trusted_solving ? "on-the-fly LRAT checking" : "LRAT proof production";
		LOGGER(_logger, V3_VERB, "Initializing rank=%i size=%i DI=%i #C=%ld with %s\n",
			solverRank, maxNumSolvers, getDiversificationIndex(), setup.numOriginalClauses,
			descriptor);

		bool okay;
		okay = solver->set("lrat", 1); assert(okay); // enable LRAT proof logging
		okay = solver->set("lratsolverid", solverRank); assert(okay); // set this solver instance's ID
		okay = solver->set("lratsolvercount", maxNumSolvers); assert(okay); // set # solvers
		okay = solver->set("lratorigclscount", setup.numOriginalClauses); assert(okay);

		if (_do_trusted_solving) {
			_trusted_solving.reset(new TrustedSolving(_logger, _setup, setup.numVars));
			_trusted_solving->init(_setup.sigFormula);
			okay = solver->set("signsharedcls", 1); assert(okay);
			solver->trace_proof_internally(
				[&](unsigned long id, const int* lits, int nbLits, const unsigned long* hints, int nbHints, uint8_t* sigData, int& sigSize) {
					return _trusted_solving->produceClause(id, lits, nbLits, hints, nbHints, sigData, sigSize);
				},
				[&](unsigned long id, const int* lits, int nbLits, const uint8_t* sigData, int sigSize) {
					return _trusted_solving->importClause(id, lits, nbLits, sigData, sigSize);
				},
				[&](const unsigned long* ids, int nbIds) {
					return _trusted_solving->deleteClauses(ids, nbIds);
				}
			);
		} else {
			okay = solver->set("binary", 1); assert(okay); // set proof logging mode to binary format
			okay = solver->set("lratdeletelines", 0); assert(okay); // disable printing deletion lines
			proofFileString = _setup.proofDir + "/proof." + std::to_string(_setup.globalId) + ".lrat";
			okay = solver->trace_proof(proofFileString.c_str()); assert(okay);
		}
	}
}

void Cadical::addLiteral(int lit) {
	if (_do_trusted_solving) _trusted_solving->loadLiteral(lit);
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

	if (_setup.diversifyNative) {
		switch (getDiversificationIndex() % getNumOriginalDiversifications()) {
		// Greedy 10-portfolio according to tests of the above configurations on SAT2020 instances
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
		assert(okay);
	}
}

int Cadical::getNumOriginalDiversifications() {
	return 10;
}

void Cadical::setPhase(const int var, const bool phase) {
	solver->phase(phase ? var : -var);
}

// Solve the formula with a given set of assumptions
// return 10 for SAT, 20 for UNSAT, 0 for UNKNOWN
SatResult Cadical::solve(size_t numAssumptions, const int* assumptions) {

	if (_do_trusted_solving) _trusted_solving->endLoading();

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
	for (size_t i = 0; i < numAssumptions; i++) {
		int lit = assumptions[i];
		solver->assume(lit);
		this->assumptions.push_back(lit);
	}

	// start solving
	int res = solver->solve();

	// Flush solver logs
	_logger.flush();
	if (ClauseMetadata::enabled()) {
		solver->flush_proof_trace ();
		solver->close_proof_trace ();
	}

	switch (res) {
	case 0:
		return UNKNOWN;
	case 10:
		return SAT;
	case 20:
		if (_do_trusted_solving) _trusted_solving->validateUnsat();
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
}

void Cadical::setSolverSuspend() {
    terminator.setSuspend();
}

void Cadical::unsetSolverSuspend() {
    terminator.unsetSuspend();
}

std::vector<int> Cadical::getSolution() {
	std::vector<int> result = {0};

	for (int i = 1; i <= getVariablesCount(); i++)
		result.push_back(solver->val(i));

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
	solver.reset();
}
