#ifndef MSCHICK_BASE_CUBE_SAT_JOB_H
#define MSCHICK_BASE_CUBE_SAT_JOB_H

#include <atomic>
#include <memory>
#include <thread>

#include "../console_horde_interface.hpp"
#include "app/job.hpp"
#include "cube_communicator.hpp"
#include "cube_lib.hpp"
#include "util/sys/threading.hpp"

class BaseCubeSatJob : public Job {
   private:
    ConsoleHordeInterface _logger;

    CubeCommunicator _cube_comm;

    std::unique_ptr<CubeLib> _lib;

    Mutex _manipulation_mutex;

    std::atomic_bool _abort_before_initialization{false};

    // Flag that signals if the CubeLib was succesfully initialized
    // Set during appl_initialize
    std::atomic_bool _isInitialized{false};
    // Flag that signals if the CubeWorker was started
    // Set during appl_initialize
    std::atomic_bool _isWorking{false};
    // Flag that signals if this job was suspended
    std::atomic_bool _isSuspended{false};
    // Flag that signals if the withdraw thread was started
    std::atomic_bool _isWithdrawing{false};
    // Flag that signals if the job may be destructed
    std::atomic_bool _isDestructible{false};

    std::thread _withdraw_thread;

    void withdraw();

    std::string getIdentifier() { return "<c-" + std::string(toStr()) + ">"; }
    std::string getLogfileSuffix() { return std::string(toStr()); };

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