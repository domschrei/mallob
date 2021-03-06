#ifndef MSCHICK_BASE_CUBE_SAT_JOB_H
#define MSCHICK_BASE_CUBE_SAT_JOB_H

#include <atomic>
#include <memory>
#include <thread>

#include "cube_job_logger.hpp"
#include "app/job.hpp"
#include "cube_communicator.hpp"
#include "cube_lib.hpp"
#include "util/sys/threading.hpp"

class BaseCubeSatJob : public Job {
   private:
    CubeJobLogger _logger;

    CubeCommunicator _cube_comm;

    std::unique_ptr<CubeLib> _lib;

    // Termination flag
    SatResult _sat_result = UNKNOWN;

    // Lifecycle of a BaseCubeSatJob
    enum State {
        UNINITIALIZED,
        INITIALIZING, // In a none root node, this state is never seen from the outside
        ACTIVE,
        WITHDRAWING,
        DESTRUCTABLE,
    };
    // TODO The state may not need to be atomic since it is only altered while hold the initialization mutex
    std::atomic<State> _job_state{UNINITIALIZED};

    // Mutex to guarantee mutual exclusion of the initializer thread and the thread controlling the job
    Mutex _initialization_mutex;

    // TODO These flag may do not need to be atomic since they are only accessed while holding the initialization mutex
    // Flag that signals if this job was interrupted
    std::atomic_bool _isInterrupted{false};
    // Flag that signals if this job was suspended
    std::atomic_bool _isSuspended{false};

    // Encapsulates behavior of appl_interrupt and appl_withdraw
    // Needs to be removed later when the behavior of them differs
    void interrupt_and_start_withdrawing();

    // Withdraws worker
    std::thread _withdraw_thread;
    void withdraw();

    // Performance
    double _working_since = 0.0;
    double _working_duration = 0.0;

    double _suspended_since = 0.0;
    double _suspended_duration = 0.0;

   public:
    BaseCubeSatJob(Parameters& params, int commSize, int worldRank, int jobId);
    ~BaseCubeSatJob() override;

    bool appl_initialize() override;
    bool appl_doneInitializing() override;
    void appl_updateRole() override;
    void appl_updateDescription(int fromRevision) override;
    void appl_pause() override;
    void appl_unpause() override;
    void appl_interrupt() override;
    void appl_withdraw() override;
    int appl_solveLoop() override;

    bool appl_wantsToBeginCommunication() const override;
    void appl_beginCommunication() override;
    void appl_communicate(int source, JobMessage& msg) override;

    void appl_dumpStats() override;
    bool appl_isDestructible() override;

    int getDemand(int prevVolume, float elapsedTime = Timer::elapsedSeconds()) const override;
};

#endif /* MSCHICK_BASE_CUBE_SAT_JOB_H */
