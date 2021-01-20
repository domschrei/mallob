#ifndef MSCHICK_CUBE_SOLVER_THREAD_H
#define MSCHICK_CUBE_SOLVER_THREAD_H

#include <atomic>
#include <thread>
#include <optional>

#include "app/sat/hordesat/solvers/cadical.hpp"
#include "dynamic_cube_setup.hpp"
#include "cube_solver_thread_manager_interface.hpp"

class CubeSolverThread {
   private:
    CubeSolverThreadManagerInterface &_manager;

    std::shared_ptr<std::vector<int>> _formula;

    LoggingInterface &_logger;

    SatResult &_result;

    std::unique_ptr<PortfolioSolverInterface> _solver;

    // The cube to solve
    // This is optional since it is set from the outside and needs to be possibly empty
    std::optional<Cube> _cube;

    // The found failed assumptions
    // This is optional since it is optionally filled during solve()
    std::optional<Cube> _failed;

    // Buffer for new failed cubes
    // They are added before every call to solve
    std::vector<int> _new_failed_cubes;

    // Mutex that protects access to the new failed cubes
    Mutex _new_failed_cubes_lock;

    std::thread _thread;

    std::atomic_bool _isInterrupted{false};

    void run();
    void solve();

   public:
    CubeSolverThread(CubeSolverThreadManagerInterface &worker, DynamicCubeSetup &setup);
    ~CubeSolverThread();

    void start();
    void interrupt();
    void join();

    void handleFailed(const std::vector<int> &failed);
};

#endif /* MSCHICK_CUBE_SOLVER_THREAD_H */