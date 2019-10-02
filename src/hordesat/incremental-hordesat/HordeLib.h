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
#include "solvers/MiniSat.h"
#include "solvers/Lingeling.h"
#include "solvers/DepQBF.h"
#include "sharing/AllToAllSharingManager.h"
#include "sharing/LogSharingManager.h"
#include "sharing/AsyncRumorSharingManager.h"

#include <pthread.h>
#include <vector>
#include <set>
#include <map>

using namespace std;

class HordeLib {
private:
	int mpi_size;
	int mpi_rank;
	Thread** solverThreads;
	bool(*endingFunction)(int,int,bool);
	size_t sleepInt;
	int solversCount;
	bool solvingDoneLocal;
	SharingManagerInterface* sharingManager;
	vector<PortfolioSolverInterface*> solvers;
    bool running;

	SatResult finalResult;
	vector<int> assumptions;
	vector<int> truthValues;
	set<int> failedAssumptions;

    double startSolving;
    int maxSeconds;
	size_t maxRounds;
	size_t round;

	//void stopAllSolvers();

	// diversifications
	void sparseDiversification(int mpi_size, int mpi_rank);
	void randomDiversification(unsigned int seed);
	void sparseRandomDiversification(unsigned int seed, int mpi_size);
	void nativeDiversification(int mpi_rank, int mpi_size);
	void binValueDiversification(int mpi_size, int mpi_rank);

    void init();

	Mutex interruptLock;
    ConditionVariable interruptCond;
    bool solversInterrupted = false;

public:

	friend void* solverRunningThread(void*);

	// settings
	ParameterProcessor params;

	// methods
	HordeLib(int argc, char** argv);
    HordeLib(const std::map<std::string, std::string>& params);
	virtual ~HordeLib();

	// dismspec solving
	bool readDimspecFile(const char* filename);
	int solveDimspec();

	// sat/qbf solving
	bool readFormula(const char* filename);
    bool setFormula(const std::vector<int>& formula); // NEW 2019-09
	void addLit(int lit);
	void assume(int lit);

    int beginSolving();
    int solveLoop();

    std::vector<int> prepareSharing(int size);
    void digestSharing(const std::vector<int>& result);
		std::vector<int> clauseBufferToPlainClauses(const std::vector<int>& buffer);

    int finishSolving();
    void interrupt();
    inline bool isRunning() {return running;};
    void setPaused();
    void unsetPaused();
    void waitUntilResumed();
    void setTerminate();

    // old
    int solve();

	int value(int lit);
	int failed(int lit);
	ParameterProcessor& getParams() {
		return params;
	}

};

#endif /* HORDELIB_H_ */
