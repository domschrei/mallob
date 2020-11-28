#ifndef MSCHICK_CUBE_SOLVER_THREAD_H
#define MSCHICK_CUBE_SOLVER_THREAD_H

#include <atomic>
#include <thread>

#include "app/sat/hordesat/solvers/cadical.hpp"
#include "cube_setup.hpp"
#include "cube_solver_thread_manager_interface.hpp"

class CubeSolverThread {
   private:
    CubeSolverThreadManagerInterface &_worker;

    std::shared_ptr<std::vector<int>> _formula;

    LoggingInterface &_logger;

    std::unique_ptr<PortfolioSolverInterface> _solver;

    std::vector<Cube> _cubes;
    std::vector<Cube> _failed_cubes;

    SatResult &_result;

    std::thread _thread;

    std::atomic_bool _isInterrupted{false};

    void run();
    void solve();

    // Helper to determine whether a given cube includes a failed cube
    bool includesFailedCube(Cube &cube);

   public:
    CubeSolverThread(CubeSolverThreadManagerInterface &worker, CubeSetup &setup);
    ~CubeSolverThread();

    void start();
    void interrupt();

    void suspend();
    void unsuspend();
};

#endif /* MSCHICK_CUBE_SOLVER_THREAD_H */