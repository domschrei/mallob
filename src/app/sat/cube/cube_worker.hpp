#ifndef MSCHICK_CUBE_WORKER_H
#define MSCHICK_CUBE_WORKER_H

#include <atomic>
#include <memory>
#include <vector>
#include <thread>

#include "app/sat/hordesat/solvers/portfolio_solver_interface.hpp"
#include "app/sat/hordesat/utilities/logging_interface.hpp"
#include "cube.hpp"
#include "cube_communicator.hpp"
#include "util/sys/threading.hpp"

class CubeWorker {
   private:
    std::vector<int> &_formula;

    CubeCommunicator &_cube_comm;

    // Termination flag (no atomic needed)
    SatResult &_result;

    // Worker thread
    std::thread _worker_thread;

    enum State {
        IDLING,
        WAITING,
        REQUESTING,
        WORKING,
        FAILED,
        RETURNING,
        SOLVED,
        FINISHED
    };
    std::atomic<State> _worker_state{State::IDLING};

    std::vector<Cube> _local_cubes;

    std::unique_ptr<LoggingInterface> _logger;
    std::unique_ptr<PortfolioSolverInterface> _solver;

    Mutex _state_mutex;
    ConditionVariable _state_cond;

    std::atomic_bool _isInterrupted{false};

    void mainLoop();
    SatResult solve();

    void digestSendCubes(std::vector<Cube> cubes);
    void digestReveicedFailedCubes();

   public:
    CubeWorker(std::vector<int> &formula, CubeCommunicator &cube_comm, SatResult &result);

    // Starts worker thread
    void startWorking();

    void interrupt();
    void join();

    bool wantsToCommunicate();
    void beginCommunication();
    void handleMessage(int source, JobMessage &msg);
};

#endif /* MSCHICK_CUBE_WORKER_H */