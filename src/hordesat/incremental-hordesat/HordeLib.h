/*
 * HordeLib.h
 *
 *  Created on: Mar 24, 2017
 *      Author: balyo
 */

#ifndef HORDELIB_H_
#define HORDELIB_H_

#include "utilities/ParameterProcessor.h"
#include "utilities/Threading.h"
#include "utilities/verbose_mutex.h"
#include "utilities/logging_interface.h"
#include "solvers/MiniSat.h"
#include "solvers/Lingeling.h"
#include "sharing/AllToAllSharingManager.h"
#include "solvers/solver_thread.h"
#include "solvers/solving_state.h"

#include <pthread.h>
#include <vector>
#include <memory>
#include <set>
#include <map>

using namespace std;

struct thread_args {
	int solverId;
	HordeLib* hlib;
	bool readFormulaFromHlib;
};

class HordeLib {
private:
	int mpi_size;
	int mpi_rank;
	Thread** solverThreads;
	size_t sleepInt;
	int solversCount;
	SharingManagerInterface* sharingManager;
	
	SolvingStates::SolvingState solvingState;
	
	vector<PortfolioSolverInterface*> solvers;
	vector<bool> solverThreadsRunning;
	vector<bool> solverThreadsInitialized;

	std::vector<std::shared_ptr<std::vector<int>>> formulae;
	std::shared_ptr<vector<int>> assumptions;

	SatResult finalResult;
	vector<int> truthValues;
	set<int> failedAssumptions;

    double startSolving;
    int maxSeconds;
	size_t maxRounds;
	size_t round;

	std::shared_ptr<LoggingInterface> logger;

	VerboseMutex solutionLock;
	VerboseMutex solvingStateLock;
	ConditionVariable stateChangeCond;
	
	// settings
	ParameterProcessor params;

public:
	friend class SolverThread;

	// methods
	HordeLib(int argc, char** argv);
    HordeLib(const std::map<std::string, std::string>& params, std::shared_ptr<LoggingInterface> loggingInterface = NULL);
	virtual ~HordeLib();

	void setLogger(std::shared_ptr<LoggingInterface> loggingInterface);
	ParameterProcessor& getParams() {return params;}

	void diversify(int rank, int size);
    void beginSolving(const std::vector<std::shared_ptr<std::vector<int>>>& formulae, 
							const std::shared_ptr<std::vector<int>>& assumptions);
	void continueSolving(const std::vector<std::shared_ptr<std::vector<int>>>& formulae, 
							const std::shared_ptr<std::vector<int>>& assumptions);
	bool isFullyInitialized();
    int solveLoop();

    std::vector<int> prepareSharing();
    void digestSharing(const std::vector<int>& result);

    int finishSolving();
    void interrupt();
	void setSolvingState(SolvingStates::SolvingState state);
    void setPaused();
    void unsetPaused();
	void abort();

	void dumpStats();

	int value(int lit);
	int failed(int lit);
	std::vector<int>& getTruthValues() {
		return truthValues;
	}
	std::set<int>& getFailedAssumptions() {
		return failedAssumptions;
	}
	

private:
    void init();

	// diversifications
	void sparseDiversification(int mpi_size, int mpi_rank);
	void randomDiversification(unsigned int seed);
	void sparseRandomDiversification(unsigned int seed, int mpi_size);
	void nativeDiversification(int mpi_rank, int mpi_size);
	void binValueDiversification(int mpi_size, int mpi_rank);
	
	std::vector<int> clauseBufferToPlainClauses(const std::vector<int>& buffer);
};

#endif /* HORDELIB_H_ */
