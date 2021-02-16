#ifndef MSCHICK_DYNAMIC_CUBE_SAT_JOB_H
#define MSCHICK_DYNAMIC_CUBE_SAT_JOB_H

#include <atomic>
#include <memory>
#include <thread>

#include "app/job.hpp"
#include "cube_job_logger.hpp"
#include "dynamic_cube_lib.hpp"
#include "util/sys/threading.hpp"

class DynamicCubeSatJob : public Job {
   private:
    CubeJobLogger _logger;

    void* _dynamic_cube_comm = NULL;  // DynamicCubeCommunicator instance (avoiding fwd decl.)
    void* _failed_assumption_comm = NULL;  // FailedAssumptionCommunicator instance (avoiding fwd decl.)

    std::unique_ptr<DynamicCubeLib> _lib;

    // Termination flag
    SatResult _sat_result = UNKNOWN;

    // Lifecycle of a DynamicCubeSatJob
    enum State {
        UNINITIALIZED,
        SUSPENDED_BEFORE_INITIALIZATION,
        INTERRUPTED_BEFORE_INITIALIZATION,
        WORKING,
        SUSPENDED,
        WITHDRAWING,
        WITHDRAWN,
    };
    // TODO The state may not need to be atomic since it is only altered while hold the initialization mutex
    std::atomic<State> _job_state{UNINITIALIZED};

    // Mutex to guarantee mutual exclusion of the initializer thread and the thread controlling the job
    Mutex _initialization_mutex;

    // Encapsulates behavior of appl_interrupt and appl_withdraw
    // Needs to be removed later when the behavior of them differs
    void interrupt_and_start_withdrawing();

    // Withdraws worker
    std::thread _withdraw_thread;
    void withdraw();

    // Variables for dynamic cube communicator
    float _time_of_last_comm = 0;
    float _job_comm_period;

    void dumpStats(long tid);

   public:
    DynamicCubeSatJob(Parameters& params, int commSize, int worldRank, int jobId);
    ~DynamicCubeSatJob() override;

    // Methods required by the dynamic cube communicator

    bool isRequesting();
    std::vector<Cube> getCubes(int bias);
    void digestCubes(std::vector<Cube>& cubes);
    std::vector<Cube> releaseAllCubes();

    // Methods required by the failed assumption communicator

    std::vector<int> getFailedAssumptions();
    void digestFailedAssumptions(std::vector<int> &failed_assumptions);

    // Methods common to all Job instances

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

#endif /* MSCHICK_DYNAMIC_CUBE_SAT_JOB_H */
