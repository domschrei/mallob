/*
 * HordeLib.cpp
 *
 *  Created on: Mar 24, 2017
 *      Author: balyo
 */

#include "HordeLib.h"
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

#include "utilities/DebugUtils.h"
#include "utilities/mympi.h"
#include "utilities/Logger.h"
#include "utilities/default_logging_interface.h"

#include <stdio.h>
#include <stdlib.h>
#include <algorithm>
#include <csignal>
#include <unistd.h>
#include <sched.h>
#include <assert.h>

using namespace SolvingStates;

HordeLib::HordeLib(int argc, char** argv) {

    params.init(argc, argv);
    init();
}

HordeLib::HordeLib(const std::map<std::string, std::string>& map, std::shared_ptr<LoggingInterface> loggingInterface) {
	
	if (loggingInterface != NULL) {
		setLogger(loggingInterface);
	}

    for (auto it = map.begin(); it != map.end(); ++it) {
        params.setParam(it->first.c_str(), it->second.c_str());
    }
    init();
}

void HordeLib::init() {

	if (logger == NULL) {
		std::shared_ptr<LoggingInterface> logInterface = std::shared_ptr<LoggingInterface>(
			new DefaultLoggingInterface(params.getIntParam("v", 1), std::string(params.getParam("jobstr", "")))
		);
		setLoggingInterface(logInterface);
	}

    startSolving = getTime();

	sharingManager = NULL;
    solvingState = INITIALIZING;
	
    // Set MPI size and rank by params or otherwise by MPI calls
	MPI_Comm_rank(MPI_COMM_WORLD, &mpi_rank);
	MPI_Comm_size(MPI_COMM_WORLD, &mpi_size);
    mpi_rank = params.getIntParam("mpirank", mpi_rank);
    mpi_size = params.getIntParam("mpisize", mpi_size);

	//if (mpi_rank == 0) setVerbosityLevel(3);

	char hostname[1024];
	gethostname(hostname, 1024);
	hlog(0, "HordeSat ($Revision: 160$) on host %s, job %s, params: ",
			hostname, params.getParam("jobstr").c_str());
	params.printParams();

	solversCount = params.getIntParam("c", 1);
	solverThreads.resize(solversCount);
	//printf("solvers is %d", solversCount);

	for (int i = 0; i < solversCount; i++) {
		if (params.getParam("s") == "minisat") {
			solvers.push_back(new MiniSat());
			hlog(3, "Running MiniSat on core %d\n", i, mpi_rank, mpi_size);
		} else if (params.getParam("s") == "combo") {
			if ((mpi_rank + i) % 2 == 0) {
				solvers.push_back(new MiniSat());
				hlog(3, "Running MiniSat on core %d\n", i, mpi_rank, mpi_size);
			} else {
				solvers.push_back(new Lingeling());
				hlog(3, "Running Lingeling on core %d\n", i, mpi_rank, mpi_size);
			}
		} else {
            Lingeling *lgl = new Lingeling();
			solvers.push_back(lgl);
			hlog(3, "Running Lingeling on core %d\n", i, mpi_rank, mpi_size);
		}
		// set solver id
		solvers[i]->solverId = i + solversCount * mpi_rank;
		
		solverThreadsInitialized.push_back(false);
		solverTids.push_back(-1);
	}

	sleepInt = 1000 * params.getIntParam("i", 1000);

	int exchangeMode = params.getIntParam("e", 1);
	sharingManager = NULL;
	if (exchangeMode == 0) {
		hlog(3, "Clause sharing disabled.\n");
	} else {
		sharingManager = new DefaultSharingManager(mpi_size, mpi_rank, solvers, params);
		hlog(3, "Initialized all-to-all clause sharing.\n");
	}
}

void HordeLib::setLogger(std::shared_ptr<LoggingInterface> loggingInterface) {
	this->logger = loggingInterface;
	setLoggingInterface(loggingInterface);
}

void* solverRunningThread(void* arg) {
	thread_args* targs = (thread_args*) arg;
    SolverThread thread(arg);
	thread.run();
	return NULL; 
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

	finalResult = UNKNOWN;

	maxSeconds = params.getIntParam("t", 0);
	maxRounds = params.getIntParam("r", 0);
	round = 1;

	for (int i = 0; i < solversCount; i++) {
        //hlog(1, "initializing solver %i.\n", i);
		thread_args* arg = new thread_args();
		arg->hlib = this;
		arg->solverId = i;
		arg->readFormulaFromHlib = true;
		solverThreads[i] = std::thread(solverRunningThread, arg);
        //hlog(1, "initialized solver %i.\n", i);
	}

	{
		auto lock = solvingStateLock.getLock();
		setSolvingState(ACTIVE);
	}
	startSolving = getTime() - startSolving;
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
	auto lock = solvingStateLock.getLock();
	setSolvingState(ACTIVE);
}

void HordeLib::updateRole(int rank, int numNodes) {
	mpi_rank = rank;
	mpi_size = numNodes;
}

bool HordeLib::isFullyInitialized() {
	if (solvingState == INITIALIZING) return false;
	for (size_t i = 0; i < solverThreadsInitialized.size(); i++) {
		if (!solverThreadsInitialized[i])
			return false;
	}
	return true;
}

int HordeLib::solveLoop() {

	setLogger(logger);

    double timeNow = getTime();
	// Sleeping?
    if (sleepInt > 0) {
        usleep(sleepInt);
    }

    // Solving done?
	bool standby = (solvingState == STANDBY);
	if (standby) {
		hlog(0, "Returning result\n");
		return finalResult;
	} 

	// Resources exhausted?
    if ((maxRounds != 0 && round == maxRounds) || (maxSeconds != 0 && timeNow > maxSeconds)) {
		hlog(0, "Aborting: round %i, time %3.3f\n", round, timeNow);
		auto lock = solvingStateLock.getLock();
        setSolvingState(STANDBY);
    }
    //fflush(stdout);
    round++;

    return -1; // no result yet
}

std::vector<int> HordeLib::prepareSharing(int maxSize) {
    assert(sharingManager != NULL);
	hlog(3, "collecting clauses on this node\n");
	std::vector<int> clauses = sharingManager->prepareSharing(maxSize);
	return clauses;
}

void HordeLib::digestSharing(const std::vector<int>& result) {
    assert(sharingManager != NULL);
	sharingManager->digestSharing(result);
}

std::vector<int> HordeLib::clauseBufferToPlainClauses(const vector<int>& buffer) {
	std::vector<int> clauses;

	int pos = 0;
	while (pos + COMM_BUFFER_SIZE < (int) buffer.size()) {
		int bufIdx = pos % COMM_BUFFER_SIZE;

		if (buffer.size() == 0) return clauses;

		int numVipClauses = buffer[pos++];
		while (numVipClauses > 0) {
			int lit = buffer[pos++];
			clauses.push_back(lit);
			if (lit == 0) {
				numVipClauses--;
			}
		}

		int clauseLength = 1;
		while (pos % COMM_BUFFER_SIZE == bufIdx) {
			int numClausesOfLength = buffer[pos++];
			if (numClausesOfLength > 0)
			for (int n = 0; n < numClausesOfLength; n++) {
				for (int i = 0; i < clauseLength; i++) {
					int lit = buffer[pos++];
					assert(lit != 0);
					clauses.push_back(lit);
				}
				clauses.push_back(0);
			}
			clauseLength++;
		}

		pos = (bufIdx + 1) * COMM_BUFFER_SIZE;
	}
	
	return clauses;
}

void HordeLib::setPaused() {
    auto lock = solvingStateLock.getLock();
	if (solvingState == ACTIVE)	setSolvingState(SUSPENDED);
}

void HordeLib::unsetPaused() {
    auto lock = solvingStateLock.getLock();
	if (solvingState == SUSPENDED) setSolvingState(ACTIVE);
}

void HordeLib::interrupt() {
	auto lock = solvingStateLock.getLock();
	if (solvingState != STANDBY) setSolvingState(STANDBY);
}

void HordeLib::abort() {
	auto lock = solvingStateLock.getLock();
	if (solvingState != ABORTING) setSolvingState(ABORTING);
}

void HordeLib::setSolvingState(SolvingState state) {
	SolvingState oldState = this->solvingState;
	this->solvingState = state;
	setLogger(logger);

	hlog(2, "state transition %s -> %s\n", SolvingStateNames[oldState], SolvingStateNames[state]);
	
	// (1) and (4) may co-occur when STANDBY -> ABORTING: 
	// Must set interruption signal _before_ waking up solvers!

	// (1) To STANDBY|ABORTING : Interrupt solvers
	// (set signal to jump out of solving procedure)
	if (state == STANDBY || state == ABORTING) {
		for (int i = 0; i < solversCount; i++) {
            if (solvers[i] != NULL) solvers[i]->setSolverInterrupt();
        }
	}
	// (2) From STANDBY to !STANDBY : Restart solvers
	else if (oldState == STANDBY && state != STANDBY) {
		for (int i = 0; i < solversCount; i++) {
            if (solvers[i] != NULL) solvers[i]->unsetSolverInterrupt();
        }
	}

	// (3) From !SUSPENDED to SUSPENDED : Suspend solvers 
	// (set signal to sleep inside solving procedure)
	if (oldState != SUSPENDED && state == SUSPENDED) {
		for (int i = 0; i < solversCount; i++) {
			if (solvers[i] != NULL) solvers[i]->setSolverSuspend();
        }
	}
	// (4) From SUSPENDED to !SUSPENDED : Resume solvers
	// (set signal to wake up and resume solving procedure)
	if (oldState == SUSPENDED && state != SUSPENDED) {
		for (int i = 0; i < solversCount; i++) {
            if (solvers[i] != NULL) solvers[i]->unsetSolverSuspend();
        }
	}

	// Signal state change
	stateChangeCond.notify();
	//pthread_cond_broadcast(stateChangeCond.get());
}

int HordeLib::finishSolving() {

	assert(solvingState == STANDBY);
	if (params.isSet("stats")) {
		dumpStats();
	}
	return finalResult;
}

void HordeLib::dumpStats() {
	// Local statistics
	SolvingStatistics locSolveStats;
	for (int i = 0; i < solversCount; i++) {
		SolvingStatistics st = solvers[i]->getStatistics();
		hlog(1, "S%d pps:%lu decs:%lu cnfs:%lu mem:%0.2f\n",
				solvers[i]->solverId, st.propagations, st.decisions, st.conflicts, st.memPeak);
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
	hlog(1, "slv:%d res:%d pps:%lu decs:%lu cnfs:%lu mem:%0.2f shrd:%lu fltd:%lu\n",
			finalResult != 0, finalResult, locSolveStats.propagations, locSolveStats.decisions,
			locSolveStats.conflicts, locSolveStats.memPeak, locShareStats.sharedClauses, locShareStats.filteredClauses);
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
	logger->log(verbosityLevel, fmt, vl);
	va_end(vl);
}

HordeLib::~HordeLib() {

	double time = getTime();

	hlog(3, "entering destructor\n");

	// for any running threads left:
	solvingStateLock.lock();
	setSolvingState(ABORTING);
	solvingStateLock.unlock();
	
	// join threads
	for (int i = 0; i < solverThreads.size(); i++) {
		if (solverThreads[i].joinable()) {
			solverThreads[i].join();
		}
	}
	hlog(3, "threads joined\n");
	solversCount = 0;

	// delete solvers
	for (int i = 0; i < solvers.size(); i++) {
		if (solvers[i] != NULL) {
			delete solvers[i];
			solvers[i] = NULL;
		}
	}
	hlog(3, "solvers deleted\n");

	// delete sharing manager
	if (sharingManager != NULL) {
		delete sharingManager;
		sharingManager = NULL;
	}

	// release formulae
	for (auto f : formulae) {
		f = NULL;
	}

	time = getTime() - time;
	setLogger(logger);
	hlog(0, "leaving destructor, took %.3f s\n", time);
}