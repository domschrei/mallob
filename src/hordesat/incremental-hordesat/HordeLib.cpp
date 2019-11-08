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

    solverThreads = NULL;
	sharingManager = NULL;
    solvingState = INITIALIZING;
	solutionLock = Mutex();
	solvingStateLock = Mutex();
	stateChangeCond = ConditionVariable();
	
    // Set MPI size and rank by params or otherwise by MPI calls
	MPI_Comm_rank(MPI_COMM_WORLD, &mpi_rank);
	MPI_Comm_size(MPI_COMM_WORLD, &mpi_size);
    mpi_rank = params.getIntParam("mpirank", mpi_rank);
    mpi_size = params.getIntParam("mpisize", mpi_size);

	//if (mpi_rank == 0) setVerbosityLevel(3);

	char hostname[1024];
	gethostname(hostname, 1024);
	log(0, "Initializing HordeSat ($Revision: 160$) on host %s on job %s with parameters: ",
			hostname, params.getParam("jobstr").c_str());
	params.printParams();

	solversCount = params.getIntParam("c", 1);
	//printf("solvers is %d", solversCount);

	for (int i = 0; i < solversCount; i++) {
		if (params.getParam("s") == "minisat") {
			solvers.push_back(new MiniSat());
			solversInitialized.push_back(0);
			log(1, "Running MiniSat on core %d\n", i, mpi_rank, mpi_size);
		} else if (params.getParam("s") == "combo") {
			if ((mpi_rank + i) % 2 == 0) {
				solvers.push_back(new MiniSat());
				solversInitialized.push_back(0);
				log(1, "Running MiniSat on core %d\n", i, mpi_rank, mpi_size);
			} else {
				solvers.push_back(new Lingeling());
				solversInitialized.push_back(0);
				log(1, "Running Lingeling on core %d\n", i, mpi_rank, mpi_size);
			}
		} else {
            Lingeling *lgl = new Lingeling();
			solvers.push_back(lgl);
			solversInitialized.push_back(0);
			log(1, "Running Lingeling on core %d\n", i, mpi_rank, mpi_size);
		}
		// set solver id
		solvers[i]->solverId = i + solversCount * mpi_rank;
	}

	sleepInt = 1000 * params.getIntParam("i", 1000);

	int exchangeMode = params.getIntParam("e", 1);
	sharingManager = NULL;
	if (exchangeMode == 0) {
		log(1, "Clause sharing disabled.\n");
	} else {
		sharingManager = new DefaultSharingManager(mpi_size, mpi_rank, solvers, params);
		log(1, "Initialized all-to-all clause sharing.\n");
	}

	diversify(mpi_rank, mpi_size);

	solverThreads = (Thread**) malloc (solversCount*sizeof(Thread*));

    log(1, "allocated solver threads\n");
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
		log(1, "doing sparse diversification\n");
		break;
	case 2:
		binValueDiversification(mpi_size, mpi_rank);
		log(1, "doing binary value based diversification\n");
		break;
	case 3:
		randomDiversification(2015);
		log(1, "doing random diversification\n");
		break;
	case 4:
		nativeDiversification(mpi_rank, mpi_size);
		log(1, "doing native diversification (plingeling)\n");
		break;
	case 5:
		sparseDiversification(mpi_size, mpi_rank);
		nativeDiversification(mpi_rank, mpi_size);
		log(1, "doing sparse + native diversification\n");
		break;
	case 6:
		sparseRandomDiversification(mpi_rank, mpi_size);
		log(1, "doing sparse random diversification\n");
		break;
	case 7:
		sparseRandomDiversification(mpi_rank, mpi_size);
		nativeDiversification(mpi_rank, mpi_size);
		log(1, "doing random sparse + native diversification (plingeling)\n");
		break;
	case 0:
		log(1, "no diversification\n");
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

void* solverRunningThread(void* arg) {
    SolverThread thread(arg);
	return thread.run();
}

void HordeLib::beginSolving(const std::vector<std::shared_ptr<std::vector<int>>>& formulae, 
							const std::shared_ptr<std::vector<int>>& assumptions) {
	
	for (auto vec : formulae) {
		this->formulae.push_back(vec);
	}
	this->assumptions = assumptions;

	maxSeconds = params.getIntParam("t", 0);
	maxRounds = params.getIntParam("r", 0);
	round = 1;

	for (int i = 0; i < solversCount; i++) {
        //log(1, "initializing solver %i.\n", i);
		thread_args* arg = new thread_args();
		arg->hlib = this;
		arg->solverId = i;
		arg->readFormulaFromHlib = true;
		solverThreads[i] = new Thread(solverRunningThread, arg);
        //log(1, "initialized solver %i.\n", i);
	}

	solvingStateLock.lock();
	setSolvingState(ACTIVE);
	solvingStateLock.unlock();

	startSolving = getTime() - startSolving;
	log(1, "Node %d started its solvers, initialization took %.3f seconds\n", mpi_rank, startSolving);
}

void HordeLib::continueSolving(const std::vector<std::shared_ptr<std::vector<int>>>& formulae, 
								const std::shared_ptr<std::vector<int>>& assumptions) {
	
	for (auto vec : formulae) {
		this->formulae.push_back(vec);
	}
	this->assumptions = assumptions;

	// unset standby
	solvingStateLock.lock();
	setSolvingState(ACTIVE);
	solvingStateLock.unlock();
}

bool HordeLib::isFullyInitialized() {
	if (solvingState == INITIALIZING) return false;
	for (size_t i = 0; i < solversInitialized.size(); i++) {
		if (solversInitialized[i] == 0)
			return false;
	}
	return true;
}

int HordeLib::solveLoop() {

	// Sleeping?
    double timeNow = getTime();
    if (sleepInt > 0) {
        usleep(sleepInt);
    }

    // Solving done?
	solvingStateLock.lock();
	bool standby = (solvingState == STANDBY);
	solvingStateLock.unlock();
	if (standby) return finalResult;

	// Resources exhausted?
    if ((maxRounds != 0 && round == maxRounds) || (maxSeconds != 0 && timeNow > maxSeconds)) {
		log(0, "Aborting: round %i, time %3.3f\n", round, timeNow);
		solvingStateLock.lock();
        setSolvingState(STANDBY);
		solvingStateLock.unlock();
    }
    fflush(stdout);
    round++;

    return -1; // no result yet
}

std::vector<int> HordeLib::prepareSharing() {
    assert(sharingManager != NULL);
	log(2, "Collecting clauses on this node ... \n");
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
    solvingStateLock.lock();
	if (solvingState == ACTIVE)	setSolvingState(SUSPENDED);
	solvingStateLock.unlock();
}

void HordeLib::unsetPaused() {
    solvingStateLock.lock();
	if (solvingState == SUSPENDED) setSolvingState(ACTIVE);
	solvingStateLock.unlock();
}

void HordeLib::interrupt() {
	solvingStateLock.lock();
	if (solvingState != STANDBY) setSolvingState(STANDBY);
	solvingStateLock.unlock();
}

void HordeLib::setSolvingState(SolvingState state) {
	SolvingState oldState = this->solvingState;
	this->solvingState = state;
	log(2, "state transition %s -> %s\n", SolvingStateNames[oldState], SolvingStateNames[state]);

	// Suspending solvers (stay inside solving procedure, but sleep)
	if (oldState != SUSPENDED && state == SUSPENDED) {
		for (int i = 0; i < solversCount; i++) {
            solvers[i]->setSolverSuspend();
        }
	}
	if (oldState == SUSPENDED && state != SUSPENDED) {
		for (int i = 0; i < solversCount; i++) {
            solvers[i]->unsetSolverSuspend();
        }
		// Overwrite static logging variable which may now belong to another HordeLib instance
		setLogger(logger);
	}

	// Interrupting solvers (jump out of solving procedure)
	if (state == STANDBY || state == ABORTING) {
		for (int i = 0; i < solversCount; i++) {
            solvers[i]->setSolverInterrupt();
        }
	}
	if (oldState == STANDBY && state != STANDBY) {
		for (int i = 0; i < solversCount; i++) {
            solvers[i]->unsetSolverInterrupt();
        }
	}

	// Signal state change
	pthread_cond_broadcast(stateChangeCond.get());
}

int HordeLib::finishSolving() {

	assert(solvingState == STANDBY);
    double searchTime = getTime() - startSolving;
	/*
	// TODO join solver threads?
	log(0, "Joining solver threads of index %d\n", mpi_rank);
	for (int i = 0; i < solversCount; i++) {
		solverThreads[i]->join();
	}*/

	if (params.isSet("stats")) {
		// Statistics gathering
		// Local statistics
		SolvingStatistics locSolveStats;
		for (int i = 0; i < solversCount; i++) {
			SolvingStatistics st = solvers[i]->getStatistics();
			log(1, "thread-stats node:%d/%d thread:%d/%d props:%lu decs:%lu confs:%lu mem:%0.2f\n",
					mpi_rank, mpi_size, i, solversCount, st.propagations, st.decisions, st.conflicts, st.memPeak);
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
		log(1, "node-stats node:%d/%d solved:%d res:%d props:%lu decs:%lu confs:%lu mem:%0.2f shared:%lu filtered:%lu\n",
				mpi_rank, mpi_size, finalResult != 0, finalResult, locSolveStats.propagations, locSolveStats.decisions,
				locSolveStats.conflicts, locSolveStats.memPeak, locShareStats.sharedClauses, locShareStats.filteredClauses);
		if (mpi_size > 1) {
			// Global statistics
			SatResult globalResult;
			MPI_Reduce(&finalResult, &globalResult, 1, MPI_INT, MPI_MAX, 0, MPI_COMM_WORLD);
			SolvingStatistics globSolveStats;
			MPI_Reduce(&locSolveStats.propagations, &globSolveStats.propagations, 1, MPI_UNSIGNED_LONG, MPI_SUM, 0, MPI_COMM_WORLD);
			MPI_Reduce(&locSolveStats.decisions, &globSolveStats.decisions, 1, MPI_UNSIGNED_LONG, MPI_SUM, 0, MPI_COMM_WORLD);
			MPI_Reduce(&locSolveStats.conflicts, &globSolveStats.conflicts, 1, MPI_UNSIGNED_LONG, MPI_SUM, 0, MPI_COMM_WORLD);
			MPI_Reduce(&locSolveStats.memPeak, &globSolveStats.memPeak, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
			SharingStatistics globShareStats;
			MPI_Reduce(&locShareStats.sharedClauses, &globShareStats.sharedClauses, 1, MPI_UNSIGNED_LONG, MPI_SUM, 0, MPI_COMM_WORLD);
			MPI_Reduce(&locShareStats.importedClauses, &globShareStats.importedClauses, 1, MPI_UNSIGNED_LONG, MPI_SUM, 0, MPI_COMM_WORLD);
			MPI_Reduce(&locShareStats.filteredClauses, &globShareStats.filteredClauses, 1, MPI_UNSIGNED_LONG, MPI_SUM, 0, MPI_COMM_WORLD);
			MPI_Reduce(&locShareStats.dropped, &globShareStats.dropped, 1, MPI_UNSIGNED_LONG, MPI_SUM, 0, MPI_COMM_WORLD);

			if (mpi_rank == 0) {
				log(0, "glob-stats nodes:%d threads:%d solved:%d res:%d rounds:%lu time:%.2f mem:%0.2f MB props:%.2f decs:%.2f confs:%.2f "
					"shared:%.2f imported:%.2f filtered:%.2f dropped:%.2f\n",
					mpi_size, solversCount, globalResult != 0, globalResult, round,
					searchTime, globSolveStats.memPeak,
					globSolveStats.propagations/searchTime, globSolveStats.decisions/searchTime, globSolveStats.conflicts/searchTime,
					globShareStats.sharedClauses/searchTime, globShareStats.importedClauses/searchTime, globShareStats.filteredClauses/searchTime, globShareStats.dropped/searchTime);
			}
		}
	}
	//log(0, "rank %d result:%d\n", mpi_rank, finalResult);

	return finalResult;
}

void HordeLib::dumpStats() {
	// Local statistics
	SolvingStatistics locSolveStats;
	for (int i = 0; i < solversCount; i++) {
		SolvingStatistics st = solvers[i]->getStatistics();
		log(1, "thread-stats node:%d/%d thread:%d/%d props:%lu decs:%lu confs:%lu mem:%0.2f\n",
				mpi_rank, mpi_size, i, solversCount, st.propagations, st.decisions, st.conflicts, st.memPeak);
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
	log(1, "node-stats node:%d/%d solved:%d res:%d props:%lu decs:%lu confs:%lu mem:%0.2f shared:%lu filtered:%lu\n",
			mpi_rank, mpi_size, finalResult != 0, finalResult, locSolveStats.propagations, locSolveStats.decisions,
			locSolveStats.conflicts, locSolveStats.memPeak, locShareStats.sharedClauses, locShareStats.filteredClauses);
}


int HordeLib::value(int lit) {
	return truthValues[abs(lit)];
}

int HordeLib::failed(int lit) {
	return failedAssumptions.find(lit) != failedAssumptions.end();
}



HordeLib::~HordeLib() {

	// for any running threads left:
	solvingStateLock.lock();
	setSolvingState(ABORTING);
	solvingStateLock.unlock();
	
	// join threads
	for (int i = 0; i < solversCount; i++) {
		solverThreads[i]->join();
		delete solverThreads[i];
	}

	// cleanup
	free(solverThreads);
	delete sharingManager;
}