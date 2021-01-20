#ifndef MSCHICK_CUBE_WORKER_GREEDY_H
#define MSCHICK_CUBE_WORKER_GREEDY_H

#include <memory>
#include <thread>

#include "cube_worker_interface.hpp"
#include "cube.hpp"
#include "cube_solver_thread_manager_interface.hpp"
#include "util/sys/threading.hpp"

class CubeWorkerGreedy : public CubeWorkerInterface, public CubeSolverThreadManagerInterface {
   private:
    // Get value from -t of mallob
    size_t _thread_count = 0;
    // Max number of cubes a thread takes -> -cubes-per-worker / -t
    size_t _batch_size = 1;

    // Wrap instances in unique_ptr to work around
    // https://stackoverflow.com/questions/42180500/vector-of-thread-wrapper-with-thread-bound-to-class-member-function
    std::vector<std::unique_ptr<CubeSolverThread>> _solver_threads;

    enum RequestState {
        NO_REQUEST,
        REQUESTING,
        RECEIVING_CUBES,
    };
    std::atomic<RequestState> _request_state{NO_REQUEST};

    std::vector<Cube> _local_cubes;
    std::vector<Cube> _failed_local_cubes;

    Mutex _local_cubes_mutex;
    ConditionVariable _local_cubes_cv;

    std::atomic_bool _isInterrupted{false};

    void digestSendCubes(std::vector<Cube> &cubes);

    void receiveFailed(std::vector<Cube> &failed);

   public:
    CubeWorkerGreedy(CubeSetup &setup);

    void shareCubes(std::vector<Cube> &cubes, std::vector<Cube> &failed, CubeSolverThread *thread) override;
    void shareFailed(std::vector<Cube> &failed) override;

    void startWorking() override;

    void interrupt() override;
    void join() override;

    void suspend() override;
    void resume() override;

    bool wantsToCommunicate() override;
    void beginCommunication() override;
    void handleMessage(int source, JobMessage &msg) override;
};

#endif /* MSCHICK_CUBE_WORKER_GREEDY_H */
