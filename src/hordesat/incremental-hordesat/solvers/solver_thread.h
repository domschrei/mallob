
#ifndef HORDE_MALLOB_SOLVER_THREAD_H
#define HORDE_MALLOB_SOLVER_THREAD_H

#include <sys/types.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <utility>

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
    std::string _name;
    std::tuple<int, int, int> _diversification_seed;

    PortfolioSolverInterface* solver;
    HordeLib* hlib;
    int importedLits;

public:
    SolverThread(void* args);
    ~SolverThread();
    void* run();

private:
    void init();
    
    void readFormula();
    void read(const std::vector<int>& formula, int begin);

    void diversify();
    void sparseDiversification(int mpi_size, int mpi_rank);
	void randomDiversification();
	void sparseRandomDiversification(int mpi_size);
	void nativeDiversification(int mpi_rank, int mpi_size);
	void binValueDiversification(int mpi_size, int mpi_rank);

    void runOnce();
    void waitWhile(SolvingStates::SolvingState state);
    bool cancelRun();
    bool cancelThread();
    void reportResult(int res);

    const char* toStr();

};

#endif