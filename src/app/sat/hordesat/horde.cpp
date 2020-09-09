/*
 * HordeLib.cpp
 *
 *  Created on: Mar 24, 2017
 *      Author: balyo
 */

#include "horde.hpp"
//============================================================================
// Name        : hordesat.cpp
// Author      : Tomas Balyo
// Version     : $Revision: 159 $
// Date        : $Date: 2017-03-24 13:01:16 +0100 (Fri, 24 Mar 2017) $
// Copyright   : copyright KIT
//============================================================================

/* TODO
 * Ideas for improvement:
 * - filter incomming clauses by some score, local glue? based on assigned lits in it?
 * - add probsat
 * - see slides for sat talk
 * - local clause database at the nodes - do not add each incoming
 * 	 clause to the blackbox, only store, maybe add later or delete
 * - using hashing distribute the work on clauses (each clause belongs
 *   one CPU that works with it - for simplification, is this applicable?
 * - more asynchronous communication. *
 *
 * Experiments:
 * - Look at all the learned clauses produced in a parallel solving, how big is the overlap? does high overlap
 *   signal that the solvers do redundant work (all do the same). Do they? Do we need explicit diversification?
 *
 * Further Ideas
 * - Blocked set solver (without UP, 1-watched literal do watch the small set, precompute point-of-no-return
 *   which is the last point a variable can be flipped (last time a blit).
 * - DPLL without unit propagation (1-watched literal), can learn clauses
 * - DPLL with Path Consistency (literal encoding SAT->CSP [Walsch 2012])
 * - measure how large are the subproblems during solving, is it worth to launch a new instance of a SAT solver for
 * 	 the subproblem? (simplified formula), cache optimization etc.
 */

#include "utilities/debug_utils.hpp"
#include "utilities/default_logging_interface.hpp"
#include "sharing/default_sharing_manager.hpp"
#include "solvers/cadical.hpp"
#include "solvers/lingeling.hpp"
#ifdef MALLOB_USE_RESTRICTED
#include "solvers/glucose.hpp"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <algorithm>
#include <csignal>
#include <unistd.h>
#include <sched.h>
#include <assert.h>

using namespace SolvingStates;

HordeLib::HordeLib(const Parameters& params, std::shared_ptr<LoggingInterface> loggingInterface) : params(params) {
	
	if (loggingInterface != NULL) {
		this->logger = loggingInterface;
		// TODO set logger
	}
    init();
}

void HordeLib::init() {

	if (logger == NULL) {
		logger = std::shared_ptr<LoggingInterface>(
			new DefaultLoggingInterface(params.getIntParam("v", 1), "<h-" + std::string(params.getParam("jobstr", "")) + ">")
		);
		// TODO set logger
	}

    startSolving = logger->getTime();

	sharingManager = NULL;
    solvingState = INITIALIZING;
	
    // Set MPI size and rank by params
    mpi_rank = params.getIntParam("apprank", mpi_rank);
    mpi_size = params.getIntParam("mpisize", mpi_size);

	//if (mpi_rank == 0) setVerbosityLevel(3);

	char hostname[1024];
	gethostname(hostname, 1024);
	hlog(0, "Hlib engine on host %s, job %s, params: ",
			hostname, params.getParam("jobstr").c_str());
	params.printParams();

	solversCount = params.getIntParam("t", 1);
	//printf("solvers is %d", solversCount);

	// Retrieve the string defining the cycle of solver choices, one character per solver
	// e.g. "llgc" => lingeling lingeling glucose cadical lingeling lingeling glucose ...
	std::string solverChoices = params.getParam("satsolver", "l");
	
	// These numbers become the diversifier indices of the solvers on this node
	int numLgl = 0;
	int numGlu = 0;
	int numCdc = 0;

	// Add solvers from full cycles on previous ranks
	// and from the begun cycle on the previous rank
	int numFullCycles = (mpi_rank * solversCount) / solverChoices.size();
	int begunCyclePos = (mpi_rank * solversCount) % solverChoices.size();
	for (size_t i = 0; i < solverChoices.size(); i++) {
		int* solverToAdd;
		switch (solverChoices[i]) {
		case 'l': solverToAdd = &numLgl; break;
		case 'g': solverToAdd = &numGlu; break;
		case 'c': solverToAdd = &numCdc; break;
		}
		*solverToAdd += numFullCycles + (i < begunCyclePos);
	}

	SolverSetup setup;
	setup.logger = logger.get();
	setup.jobname = params.getParam("jobstr");
	setup.useAdditionalDiversification = params.isNotNull("aod");
	setup.hardInitialMaxLbd = params.getIntParam("ihlbd");
	setup.hardFinalMaxLbd = params.getIntParam("fhlbd");
	setup.softInitialMaxLbd = params.getIntParam("islbd");
	setup.softFinalMaxLbd = params.getIntParam("fslbd");
	setup.hardMaxClauseLength = params.getIntParam("hmcl");
	setup.softMaxClauseLength = params.getIntParam("smcl");

	// Instantiate solvers according to the global solver IDs and diversification indices
	int cyclePos = begunCyclePos;
	for (setup.localId = 0; setup.localId < solversCount; setup.localId++) {
		setup.globalId = mpi_rank * solversCount + setup.localId;
		// Which solver?
		switch (solverChoices[cyclePos]) {
		case 'l':
			// Lingeling
			hlog(3, "S%i : Employing Lingeling\n", setup.globalId);
			setup.diversificationIndex = numLgl++;
			solverInterfaces.emplace_back(new Lingeling(setup));
			break;
		case 'c':
			// Cadical
			hlog(3, "S%i : Employing Cadical\n", setup.globalId);
			setup.diversificationIndex = numCdc++;
			solverInterfaces.emplace_back(new Cadical(setup));
			break;
#ifdef MALLOB_USE_RESTRICTED
		case 'g':
			// Glucose
			hlog(3, "S%i: Employing Glucose\n", setup.globalId);
			setup.diversificationIndex = numGlu++;
			solverInterfaces.emplace_back(new MGlucose(setup));
			break;
#endif
		default:
			// Invalid solver
			hlog(0, "Fatal error: Invalid solver \"%c\" assigned\n", solverChoices[cyclePos]);
			abort();
			break;
		}
		cyclePos = (cyclePos+1) % solverChoices.size();
	}

	sleepInt = 1000 * params.getIntParam("i", 1000);

	sharingManager.reset(new DefaultSharingManager(solverInterfaces, params, *logger));
	hlog(3, "Initialized all-to-all clause sharing.\n");
}

void HordeLib::beginSolving(const std::vector<std::shared_ptr<std::vector<int>>>& formulae, 
							const std::shared_ptr<std::vector<int>>& assumptions) {
	
	for (auto vec : formulae) {
		if (vec == NULL) {
			return;
		}
		this->formulae.push_back(vec);
	}
	if (assumptions != NULL) {
		this->assumptions = assumptions;
	}
	assert(this->assumptions != NULL);

	finalResult = UNKNOWN;

	maxSeconds = params.getIntParam("hmaxsecs", 0);
	maxRounds = params.getIntParam("hmaxrounds", 0);
	round = 1;

	for (int i = 0; i < solversCount; i++) {
        //hlog(1, "initializing solver %i.\n", i);
		solverThreads.emplace_back(new SolverThread(params, *logger, solverInterfaces[i], formulae, assumptions, i, &anySolutionFound));
		solverThreads.back()->start();
		//hlog(1, "initialized solver %i.\n", i);
	}
	setSolvingState(ACTIVE);
	startSolving = logger->getTime() - startSolving;
	hlog(3, "started solver threads, took %.3f seconds\n", startSolving);
}

void HordeLib::continueSolving(const std::vector<std::shared_ptr<std::vector<int>>>& formulae, 
								const std::shared_ptr<std::vector<int>>& assumptions) {
	
	for (auto vec : formulae) {
		this->formulae.push_back(vec);
	}
	this->assumptions = assumptions;
	finalResult = UNKNOWN;

	// unset standby
	setSolvingState(ACTIVE);
}

void HordeLib::updateRole(int rank, int numNodes) {
	mpi_rank = rank;
	mpi_size = numNodes;
}

bool HordeLib::isFullyInitialized() {
	if (solvingState == INITIALIZING) return false;
	for (size_t i = 0; i < solverThreads.size(); i++) {
		if (!solverThreads[i]->isInitialized()) return false;
	}
	return true;
}

int HordeLib::solveLoop() {
	if (isCleanedUp()) return -1;

    double timeNow = logger->getTime();
	// Sleeping?
    if (sleepInt > 0) {
        usleep(sleepInt);
    }

    // Solving done?
	bool done = false;
	for (size_t i = 0; i < solverThreads.size(); i++) {
		if (solverThreads[i]->getState() == STANDBY) {
			done = true;
			finalResult = solverThreads[i]->getSatResult();
			if (finalResult == SAT) {
				truthValues = solverThreads[i]->getSolution();
			} else {
				failedAssumptions = solverThreads[i]->getFailedAssumptions();
			}
		}
	}

	if (done) {
		hlog(0, "Returning result\n");
		return finalResult;
	} 

	// Resources exhausted?
    if ((maxRounds != 0 && round == maxRounds) || (maxSeconds != 0 && timeNow > maxSeconds)) {
		hlog(0, "Aborting: round %i, time %3.3f\n", round, timeNow);
        setSolvingState(STANDBY);
    }
    //fflush(stdout);
    round++;

    return -1; // no result yet
}

int HordeLib::prepareSharing(int* begin, int maxSize) {
	if (isCleanedUp()) return 0;
    assert(sharingManager != NULL);
	hlog(3, "collecting clauses on this node\n");
	return sharingManager->prepareSharing(begin, maxSize);
}

void HordeLib::digestSharing(const std::vector<int>& result) {
	if (isCleanedUp()) return;
    assert(sharingManager != NULL);
	sharingManager->digestSharing(result);
}

void HordeLib::digestSharing(int* begin, int size) {
	if (isCleanedUp()) return;
    assert(sharingManager != NULL);
	sharingManager->digestSharing(begin, size);
}

void HordeLib::dumpStats(bool final) {
	if (isCleanedUp() || !isFullyInitialized()) return;

	// Local statistics
	SolvingStatistics locSolveStats;
	for (int i = 0; i < solversCount; i++) {
		if (solverInterfaces[i] == NULL) continue;
		SolvingStatistics st = solverInterfaces[i]->getStatistics();
		hlog(0, "%sS%d pps:%lu decs:%lu cnfs:%lu mem:%0.2f\n",
				final ? "END " : "",
				solverInterfaces[i]->getGlobalId(), st.propagations, st.decisions, st.conflicts, st.memPeak);
		locSolveStats.conflicts += st.conflicts;
		locSolveStats.decisions += st.decisions;
		locSolveStats.memPeak += st.memPeak;
		locSolveStats.propagations += st.propagations;
		locSolveStats.restarts += st.restarts;
	}
	SharingStatistics locShareStats;
	if (sharingManager != NULL) {
		locShareStats = sharingManager->getStatistics();
	}
	unsigned long exportedWithFailed = locShareStats.exportedClauses + locShareStats.clausesFilteredAtExport + locShareStats.clausesDroppedAtExport;
	unsigned long importedWithFailed = locShareStats.importedClauses + locShareStats.clausesFilteredAtImport;
	hlog(0, "%spps:%lu decs:%lu cnfs:%lu mem:%0.2f exp:%lu/%lu(drp:%lu) imp:%lu/%lu\n",
			final ? "END " : "",
			locSolveStats.propagations, locSolveStats.decisions, locSolveStats.conflicts, locSolveStats.memPeak, 
			locShareStats.exportedClauses, exportedWithFailed, locShareStats.clausesDroppedAtExport, 
			locShareStats.importedClauses, importedWithFailed);
	if (final) {
		// Histogram over clause lengths
		std::string hist = "";
		for (int i = 1; i < 50; i++) {
			hist += std::to_string(locShareStats.seenClauseLenHistogram[i]) + " ";
		}
		hlog(0, "END clenhist: %s\n", hist.c_str());
	}
}

void HordeLib::setPaused() {
	if (solvingState == ACTIVE)	setSolvingState(SUSPENDED);
}

void HordeLib::unsetPaused() {
	if (solvingState == SUSPENDED) setSolvingState(ACTIVE);
}

void HordeLib::interrupt() {
	if (solvingState != STANDBY) {
		setSolvingState(STANDBY);
		dumpStats(/*final=*/true);
	}
}

void HordeLib::abort() {
	if (solvingState != ABORTING) setSolvingState(ABORTING);
}

void HordeLib::setSolvingState(SolvingState state) {
	SolvingState oldState = this->solvingState;
	this->solvingState = state;

	hlog(2, "state transition %s -> %s\n", SolvingStateNames[oldState], SolvingStateNames[state]);
	for (auto& solver : solverThreads) solver->setState(state);
}

int HordeLib::value(int lit) {
	return truthValues[abs(lit)];
}

int HordeLib::failed(int lit) {
	return failedAssumptions.find(lit) != failedAssumptions.end();
}

void HordeLib::hlog(int verbosityLevel, const char* fmt, ...) {
	va_list vl;
    va_start(vl, fmt);
	logger->log_va_list(verbosityLevel, fmt, vl);
	va_end(vl);
}

void HordeLib::cleanUp() {
	double time = logger->getTime();

	hlog(3, "[hlib-cleanup] enter\n");

	// for any running threads left:
	setSolvingState(ABORTING);
	
	// join threads
	for (size_t i = 0; i < solverThreads.size(); i++) {
		solverThreads[i]->tryJoin();
	}
	solverThreads.clear();
	hlog(3, "[hlib-cleanup] joined threads\n");

	// delete solvers
	solversCount = 0;
	for (size_t i = 0; i < solverInterfaces.size(); i++) {
		solverInterfaces[i].reset();
	}
	solverInterfaces.clear();
	hlog(3, "[hlib-cleanup] cleared solvers\n");

	// release formulae
	for (auto f : formulae) {
		f.reset();
	}

	time = logger->getTime() - time;
	hlog(2, "[hlib-cleanup] done, took %.3f s\n", time);

	cleanedUp = true;
}

HordeLib::~HordeLib() {
	assert(cleanedUp);
}