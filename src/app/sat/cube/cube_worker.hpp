#ifndef MSCHICK_CUBE_WORKER_H
#define MSCHICK_CUBE_WORKER_H

#include <memory>
#include <thread>

#include "app/sat/hordesat/utilities/logging_interface.hpp"
#include "cube.hpp"
#include "cube_worker_interface.hpp"
#include "util/sys/threading.hpp"

class CubeWorker : public CubeWorkerInterface {
   private:
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

    std::unique_ptr<PortfolioSolverInterface> _solver;

    Mutex _state_mutex;
    ConditionVariable _state_cond;

    std::atomic_bool _isInterrupted{false};

    double _time_of_last_msg = 0.0;
    double _time_waiting_for_msg = 0.0;

    void mainLoop();
    SatResult solve();

    void digestSendCubes(std::vector<Cube> cubes);
    void digestReveicedFailedCubes();

   public:
    CubeWorker(CubeSetup setup);
    ~CubeWorker();

    void startWorking() override;

    void interrupt() override;
    void join() override;

    void suspend() override;
    void resume() override;

    bool wantsToCommunicate() override;
    void beginCommunication() override;
    void handleMessage(int source, JobMessage &msg) override;
};

#endif /* MSCHICK_CUBE_WORKER_H */