
#ifndef HORDE_MALLOB_SOLVER_THREAD_H
#define HORDE_MALLOB_SOLVER_THREAD_H

#include "utilities/ParameterProcessor.h"
#include "utilities/Threading.h"
#include "utilities/logging_interface.h"
#include "solvers/PortfolioSolverInterface.h"
#include "solvers/solving_state.h"

// Forward declarations
class HordeLib;
struct thread_args;

class SolverThread {

private:
    thread_args* _args;
    PortfolioSolverInterface* solver;
    HordeLib* hlib;
    int importedLits;

public:
    SolverThread(void* args) {
        _args = (thread_args*)args;
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