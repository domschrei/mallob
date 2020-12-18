
#ifndef HORDE_MALLOB_SOLVER_THREAD_H
#define HORDE_MALLOB_SOLVER_THREAD_H

#include <sys/types.h>
#include <unistd.h>
#include <utility>
#include <thread>
#include <atomic>

#include "util/params.hpp"
#include "util/sys/threading.hpp"
#include "app/sat/hordesat/utilities/logging_interface.hpp"
#include "app/sat/hordesat/solvers/portfolio_solver_interface.hpp"
#include "app/sat/hordesat/solvers/solving_state.hpp"

// Forward declarations
class HordeLib;

class SolverThread {

private:
    const Parameters& _params;
    std::shared_ptr<PortfolioSolverInterface> _solver_ptr;
    PortfolioSolverInterface& _solver;
    std::shared_ptr<LoggingInterface> _logger;
    std::thread _thread;

    const std::vector<std::shared_ptr<std::vector<int>>>& _formulae;
    const std::shared_ptr<std::vector<int>>& _assumptions;
    
    int _local_id;
    std::string _name;
    int _portfolio_rank;
    int _portfolio_size;

    volatile SolvingStates::SolvingState _state;
    Mutex _state_mutex;
    ConditionVariable _state_cond;

    SatResult _result;
    std::vector<int> _solution;
    std::set<int> _failed_assumptions;

    size_t _imported_lits = 0;
    long _tid = -1;

    std::atomic_bool _initialized = false;
    std::atomic_bool* _finished_flag;


public:
    SolverThread(const Parameters& params, const LoggingInterface& logger,
                std::shared_ptr<PortfolioSolverInterface> solver, 
                const std::vector<std::shared_ptr<std::vector<int>>>& formulae, 
                const std::shared_ptr<std::vector<int>>& assumptions, 
                int localId, std::atomic_bool* finished);
    ~SolverThread();

    void init();
    void start();
    void setState(SolvingStates::SolvingState state);
    void tryJoin() {if (_thread.joinable()) _thread.join();}

    bool isInitialized() const {
        return _initialized;
    }
    int getTid() const {
        return _tid;
    }
    SolvingStates::SolvingState getState() const {
        return _state;
    }
    SatResult getSatResult() const {
        return _result;
    }
    const std::vector<int>& getSolution() const {
        return _solution;
    }
    const std::set<int>& getFailedAssumptions() const {
        return _failed_assumptions;
    }

private:
    void* run();
    
    void pin();
    void readFormula();
    void read(const std::vector<int>& formula, int begin);

    void diversify();
    void sparseDiversification(int mpi_size, int mpi_rank);
	void randomDiversification();
	void sparseRandomDiversification(int mpi_size);
	void nativeDiversification();
	void binValueDiversification(int mpi_size, int mpi_rank);

    void runOnce();
    void waitWhile(SolvingStates::SolvingState state);
    bool cancelRun();
    bool cancelThread();
    void reportResult(int res);

    void log(int verb, const char* fmt, ...);
    const char* toStr();

};

#endif