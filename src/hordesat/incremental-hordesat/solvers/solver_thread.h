
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

class SolverThread {

private:
    ParameterProcessor& _params;
    std::shared_ptr<PortfolioSolverInterface> _solver_ptr;
    PortfolioSolverInterface& _solver;

    std::vector<std::shared_ptr<std::vector<int>>>& _formulae;
    std::shared_ptr<vector<int>>& _assumptions;
    
    int _local_id;
    std::string _name;
    int _portfolio_rank;
    int _portfolio_size;

    volatile SolvingState _state;
    Mutex _state_mutex;
    ConditionVariable _state_cond;

    SatResult _result;
    std::vector<int> _solution;
    std::set<int> _failed_assumptions;

    int _imported_lits = 0;
    bool _initialized = false;
    long _tid = -1;


public:
    SolverThread(ParameterProcessor& params, std::shared_ptr<PortfolioSolverInterface> solver, 
                std::vector<std::shared_ptr<std::vector<int>>>& formulae, 
                std::shared_ptr<vector<int>>& assumptions, int localId);
    ~SolverThread();

    void init();
    void* run();

    bool isInitialized() {return _initialized;}
    int getTid() {return _tid;}
    void setState(SolvingState state) {

        _state_mutex.lock();
        SolvingState oldState = _state;

        // (1) To STANDBY|ABORTING : Interrupt solver
        // (set signal to jump out of solving procedure)
        if (state == STANDBY || state == ABORTING) {
            _solver.setSolverInterrupt();
        }
        // (2) From STANDBY to !STANDBY : Restart solver
        else if (oldState == STANDBY && state != STANDBY) {
            _solver.unsetSolverInterrupt();
        }

        // (3) From !SUSPENDED to SUSPENDED : Suspend solvers 
        // (set signal to sleep inside solving procedure)
        if (oldState != SUSPENDED && state == SUSPENDED) {
            _solver.setSolverSuspend();
        }
        // (4) From SUSPENDED to !SUSPENDED : Resume solvers
        // (set signal to wake up and resume solving procedure)
        if (oldState == SUSPENDED && state != SUSPENDED) {
            _solver.unsetSolverSuspend();
        }

        _state = state; 

        _state_mutex.unlock();
        _state_cond.notify();
    }
    SolvingState getState() {
        auto lock = _state_mutex.getLock();
        return _state;
    }
    SatResult getSatResult() {
        return _result;
    }
    const std::vector<int>& getSolution() {
        return _solution;
    }
    const std::set<int>& getFailedAssumptions() {
        return _failed_assumptions;
    }

private:
    
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