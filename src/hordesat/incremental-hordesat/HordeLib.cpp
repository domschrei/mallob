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
	hlog(0, "Initializing HordeSat ($Revision: 160$) on host %s on job %s with parameters: ",
			hostname, params.getParam("jobstr").c_str());
	params.printParams();

	solversCount = params.getIntParam("c", 1);
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

	diversify(mpi_rank, mpi_size);

	threadPool = new SolverThreadPool(&params, solvers);

    hlog(1, "allocated solver threads\n");
}

void HordeLib::setLogger(std::shared_ptr<LoggingInterface> loggingInterface) {
	this->logger = loggingInterface;
	setLoggingInterface(loggingInterface);
}

void HordeLib::diversify(int rank, int size) {

	int diversification = params.getIntParam("d", 1);
	switch (diversification) {
	case 1:
		sparseDiversification(mpi_size, mpi_rank);
		hlog(1, "doing sparse diversification\n");
		break;
	case 2:
		binValueDiversification(mpi_size, mpi_rank);
		hlog(1, "doing binary value based diversification\n");
		break;
	case 3:
		randomDiversification(2015);
		hlog(1, "doing random diversification\n");
		break;
	case 4:
		nativeDiversification(mpi_rank, mpi_size);
		hlog(1, "doing native diversification (plingeling)\n");
		break;
	case 5:
		sparseDiversification(mpi_size, mpi_rank);
		nativeDiversification(mpi_rank, mpi_size);
		hlog(1, "doing sparse + native diversification\n");
		break;
	case 6:
		sparseRandomDiversification(mpi_rank, mpi_size);
		hlog(1, "doing sparse random diversification\n");
		break;
	case 7:
		sparseRandomDiversification(mpi_rank, mpi_size);
		nativeDiversification(mpi_rank, mpi_size);
		hlog(1, "doing random sparse + native diversification (plingeling)\n");
		break;
	case 0:
		hlog(1, "no diversification\n");
		break;
	}
}

void HordeLib::sparseDiversification(int mpi_size, int mpi_rank) {
	int totalSolvers = mpi_size * solversCount;
    int vars = solvers[0]->getVariablesCount();
    for (int sid = 0; sid < solversCount; sid++) {
    	int shift = (mpi_rank * solversCount) + sid;
		for (int var = 1; var + totalSolvers < vars; var += totalSolvers) {
			solvers[sid]->setPhase(var + shift, true);
		}
    }
}

void HordeLib::randomDiversification(unsigned int seed) {
	srand(seed);
    int vars = solvers[0]->getVariablesCount();
    for (int sid = 0; sid < solversCount; sid++) {
		for (int var = 1; var <= vars; var++) {
			solvers[sid]->setPhase(var, rand()%2 == 1);
		}
    }
}

void HordeLib::sparseRandomDiversification(unsigned int seed, int mpi_size) {
	srand(seed);
	int totalSolvers = solversCount * mpi_size;
    int vars = solvers[0]->getVariablesCount();
    for (int sid = 0; sid < solversCount; sid++) {
		for (int var = 1; var <= vars; var++) {
			if (rand() % totalSolvers == 0) {
				solvers[sid]->setPhase(var, rand() % 2 == 1);
			}
		}
    }
}

void HordeLib::nativeDiversification(int mpi_rank, int mpi_size) {
    for (int sid = 0; sid < solversCount; sid++) {
    	solvers[sid]->diversify(solvers[sid]->solverId, mpi_size*solversCount);
    }
}

void HordeLib::binValueDiversification(int mpi_size, int mpi_rank) {
	int totalSolvers = mpi_size * solversCount;
	int tmp = totalSolvers;
	int log = 0;
	while (tmp) {
		tmp >>= 1;
		log++;
	}
    int vars = solvers[0]->getVariablesCount();
    for (int sid = 0; sid < solversCount; sid++) {
    	int num = mpi_rank * sid;
		for (int var = 1; var < vars; var++) {
			int bit = var % log;
			bool phase = (num >> bit) & 1 ? true : false;
			solvers[sid]->setPhase(var, phase);
		}
    }
}

void HordeLib::beginSolving(const std::vector<std::shared_ptr<std::vector<int>>>& formulae, 
							const std::shared_ptr<std::vector<int>>& assumptions) {
		
	maxSeconds = params.getIntParam("t", 0);
	maxRounds = params.getIntParam("r", 0);
	round = 1;

	threadPool->startAll(formulae, assumptions);
	setSolvingState(ACTIVE);

	startSolving = getTime() - startSolving;
	hlog(1, "started solver threads, took %.3f seconds\n", startSolving);
}

void HordeLib::continueSolving(const std::vector<std::shared_ptr<std::vector<int>>>& formulae, 
								const std::shared_ptr<std::vector<int>>& assumptions) {
	
	abort(); // TODO

	// unset standby
	setSolvingState(ACTIVE);
}

bool HordeLib::isFullyInitialized() {
	if (solvingState == INITIALIZING) return false;
	return threadPool->allSolversRunning();
}

int HordeLib::solveLoop() {

	setLogger(logger);

    double timeNow = getTime();
	// Sleeping?
    if (sleepInt > 0) {
        usleep(sleepInt);
    }

    // Solving done?
	SolverResult& result = threadPool->getResult();
	this->result = &result;
	if (result.finalResult != UNKNOWN) {
		hlog(0, "Returning result\n");
		return result.finalResult;
	}

	// Resources exhausted?
    if ((maxRounds != 0 && round == maxRounds) || (maxSeconds != 0 && timeNow > maxSeconds)) {
		hlog(0, "Aborting: round %i, time %3.3f\n", round, timeNow);
		// TODO return some result that makes the caller abort the instance
		return -2;
    }
    //fflush(stdout);
    round++;

    return -1; // no result yet
}

std::vector<int> HordeLib::prepareSharing() {
    assert(sharingManager != NULL);
	hlog(3, "collecting clauses on this node\n");
	std::vector<int> clauses = sharingManager->prepareSharing();
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
    if (solvingState == ACTIVE) {
		threadPool->suspendAll();
		setSolvingState(SUSPENDED);
	}	
}

void HordeLib::unsetPaused() {
	if (solvingState == SUSPENDED) {
		threadPool->unsuspendAll();
		setSolvingState(ACTIVE);
	} 
}

void HordeLib::interrupt() {
	if (solvingState != STANDBY && solvingState != ABORTING) {
		
		if (solvingState == SUSPENDED) {
			threadPool->unsuspendAll();
		}

		threadPool->interruptAll();
		setSolvingState(STANDBY);
	}
}

void HordeLib::abort() {
	if (solvingState != STANDBY && solvingState != ABORTING) {
		threadPool->interruptAll();
		setSolvingState(ABORTING);
	}
}

void HordeLib::setSolvingState(SolvingState state) {
	SolvingState oldState = this->solvingState;
	this->solvingState = state;
	setLogger(logger);

	hlog(2, "state transition %s -> %s\n", SolvingStateNames[oldState], SolvingStateNames[state]);
	
	// Suspension and interruption is now handled inside the SolverThreadPool 

	/*
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
	pthread_cond_broadcast(stateChangeCond.get());
	*/
}

int HordeLib::finishSolving() {

	assert(solvingState == STANDBY);
    double searchTime = getTime() - startSolving;

	if (params.isSet("stats")) {
		dumpStats();
	}
	//hlog(0, "rank %d result:%d\n", mpi_rank, finalResult);
	if (result == NULL) return -1;
	return result->finalResult;
}

void HordeLib::dumpStats() {
	// Local statistics
	SolvingStatistics locSolveStats;
	for (int i = 0; i < solversCount; i++) {
		SolvingStatistics st = solvers[i]->getStatistics();
		hlog(1, "S%d stats props:%lu decs:%lu confs:%lu mem:%0.2f\n",
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
	int finalResult = result->finalResult;
	hlog(1, "node-stats solved:%d res:%d props:%lu decs:%lu confs:%lu mem:%0.2f shared:%lu filtered:%lu\n",
			finalResult != 0, finalResult, locSolveStats.propagations, locSolveStats.decisions,
			locSolveStats.conflicts, locSolveStats.memPeak, locShareStats.sharedClauses, locShareStats.filteredClauses);
}


int HordeLib::value(int lit) {
	return result->truthValues[abs(lit)];
}

int HordeLib::failed(int lit) {
	return result->failedAssumptions.find(lit) != result->failedAssumptions.end();
}

std::vector<int>& HordeLib::getTruthValues() {
	return result->truthValues;
}

std::set<int>& HordeLib::getFailedAssumptions() {
	return result->failedAssumptions;	
}

void HordeLib::hlog(int verbosityLevel, const char* fmt, ...) {
	va_list vl;
    va_start(vl, fmt);
	logger->log(verbosityLevel, fmt, vl);
	va_end(vl);
}

HordeLib::~HordeLib() {

	double time = getTime();

	hlog(0, "entering destructor\n");

	// for any running threads left:
	setSolvingState(ABORTING);

	// join threads
	threadPool->joinAll();
	hlog(0, "threads joined\n");
	delete threadPool;
	hlog(0, "threads deleted\n");

	// delete solvers
	for (int i = 0; i < solvers.size(); i++) {
		if (solvers[i] != NULL) {
			delete solvers[i];
			solvers[i] = NULL;
		}
	}
	hlog(0, "solvers deleted\n");

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