#ifndef MSCHICK_CUBE_GENERATOR_THREAD_H
#define MSCHICK_CUBE_GENERATOR_THREAD_H

#include <atomic>
#include <thread>

#include "app/sat/hordesat/solvers/cadical.hpp"
#include "cube_setup.hpp"
#include "cube.hpp"

class CubeGeneratorThread {
   private:
    // CubeSolverThreadManagerInterface &_worker;

    std::shared_ptr<std::vector<int>> _formula;

    LoggingInterface &_logger;

    std::unique_ptr<PortfolioSolverInterface> _solver;

    // Cube that should be expanded
    Cube _cube;

    // Learnt failed assumptions
    std::vector<int> _failed;
    // Learnt clauses
    std::vector<int> _clauses;
    // May be these two are already added to solver and do not need storage here

    // May solve formula during lookahead
    // TODO: Add difficult handling like in cube root
    SatResult &_result;

    std::thread _thread;

    std::atomic_bool _isInterrupted{false};

    void run();
    void generate();

    // Helper to determine whether a given cube includes a failed cube
    bool includesFailedCube(Cube &cube);

   public:
    CubeGeneratorThread(CubeSolverThreadManagerInterface &worker, CubeSetup &setup);
    ~CubeGeneratorThread();

    void start();
    void interrupt();

    void learn(std::vector<int> &failed, std::vector<int> &clauses);

    void suspend();
    void unsuspend();
};

#endif /* MSCHICK_CUBE_GENERATOR_THREAD_H */