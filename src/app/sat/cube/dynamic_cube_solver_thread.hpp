#ifndef MSCHICK_CUBE_SOLVER_THREAD_H
#define MSCHICK_CUBE_SOLVER_THREAD_H

#include <atomic>
#include <optional>
#include <thread>

#include "app/sat/hordesat/solvers/cadical.hpp"
#include "dynamic_cube_setup.hpp"
#include "dynamic_cube_solver_thread_manager_interface.hpp"

class DynamicCubeSolverThread {
   private:
    DynamicCubeSolverThreadManagerInterface &_manager;

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

    static std::atomic<int> _counter;

    int _instance_counter = 0;

    size_t _added_failed_assumptions_buffer = 0;

    void run();
    void solve();

   public:
    DynamicCubeSolverThread(DynamicCubeSolverThreadManagerInterface &worker, const DynamicCubeSetup &setup);
    ~DynamicCubeSolverThread();

    void start();
    void interrupt();
    void join();

    void handleFailed(const std::vector<int> &failed);
};

#endif /* MSCHICK_CUBE_SOLVER_THREAD_H */