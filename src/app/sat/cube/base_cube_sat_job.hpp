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

    Mutex _initialization_mutex;

    std::atomic_bool _abort_before_initialization{false};

    std::atomic_bool _isInitialized{false};
    std::atomic_bool _isDestructible{false};

    std::thread _withdraw_thread;

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

    void cleanUp();
};

#endif /* MSCHICK_BASE_CUBE_SAT_JOB_H */