
#ifndef HORDE_MALLOB_SOLVER_THREAD_H
#define HORDE_MALLOB_SOLVER_THREAD_H

#include "utilities/ParameterProcessor.h"
#include "utilities/Threading.h"
#include "utilities/logging_interface.h"
#include "solvers/PortfolioSolverInterface.h"
#include "solvers/solving_state.h"

struct SolverResult {
    SatResult finalResult = UNKNOWN;
	vector<int> truthValues;
	set<int> failedAssumptions;
};

struct ThreadInfo {
    int id;
    
    PortfolioSolverInterface* solver;
    std::vector<std::shared_ptr<std::vector<int>>> formulae;
    std::shared_ptr<vector<int>> assumptions;

    // Set by thread, read-only by parent
    bool running = true;
    bool initializing = true;
    bool suspended = false;
    bool interrupted = false;
    bool finished = false;
    SolverResult result;

    // Set by parent, read-only by thread
    bool suspensionSignal = false;
    bool unsuspensionSignal = false;
    bool interruptionSignal = false;

    Mutex suspendMutex;
    ConditionVariable suspendVar;
};

class SolverThread {

private:
    ThreadInfo* _info;
    std::string _name;
    PortfolioSolverInterface* solver;
    int importedLits;

public:
    SolverThread(void* args) {
        _info = (ThreadInfo*) args;
    }
    ~SolverThread();
    void* run();

private:
    void init();
    void readFormula();
    void read(const std::vector<int>& formula, int begin);
    void runOnce();
    void waitWhile(SolvingStates::SolvingState state);
    bool cancelRun();
    bool cancelThread();
    void reportResult(int res);
    
    const char* toStr();

};

#endif