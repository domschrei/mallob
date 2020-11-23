#ifndef MSCHICK_CUBE_SOLVER_THREAD_H
#define MSCHICK_CUBE_SOLVER_THREAD_H

#include <thread>
#include <atomic>

#include "app/sat/console_horde_interface.hpp"
#include "app/sat/hordesat/solvers/cadical.hpp"
#include "app/sat/hordesat/utilities/logging_interface.hpp"
#include "cube.hpp"
#include "cube_worker_interface.hpp"

class CubeSolverThread {
   private:
    CubeWorkerInterface *_worker;

    VecPtr _formula;

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
    CubeSolverThread(CubeWorkerInterface *worker, CubeSetup &setup);
    ~CubeSolverThread();

    void start();
    void interrupt();

    void suspend();
    void unsuspend();
};

#endif /* MSCHICK_CUBE_SOLVER_THREAD_H */