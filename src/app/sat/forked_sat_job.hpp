
#ifndef DOMPASCH_CHILD_PROCESS_JOB_IMAGE_H
#define DOMPASCH_CHILD_PROCESS_JOB_IMAGE_H

#include <string>
#include <memory>
#include <thread>
#include <atomic>
#include <future>

#include "app/job.hpp"
#include "util/params.hpp"
#include "horde_process_adapter.hpp"
#include "sat_constants.h"
#include "base_sat_job.hpp"

class ForkedSatJob : public BaseSatJob {

private:
    static std::atomic_int _static_subprocess_index;
    
    std::atomic_bool _initialized = false;

    std::unique_ptr<HordeProcessAdapter> _solver;
    void* _clause_comm = nullptr; // SatClauseCommunicator instance (avoiding fwd decl.)
    int _last_imported_revision = 0;

    std::future<void> _destruction;
    std::atomic_bool _shmem_freed = false;

    float _time_of_start_solving = 0;
    float _time_of_last_comm = 0;
    float _job_comm_period;

    std::atomic_bool _done_locally = false;
    JobResult _internal_result;

public:

    ForkedSatJob(const Parameters& params, int commSize, int worldRank, int jobId, JobDescription::Application appl);
    virtual ~ForkedSatJob() override;

    void appl_start() override;
    void appl_suspend() override;
    void appl_resume() override;
    void appl_terminate() override;

    int appl_solved() override;
    JobResult&& appl_getResult() override;

    bool appl_wantsToBeginCommunication() override;
    void appl_beginCommunication() override;
    void appl_communicate(int source, JobMessage& msg) override;

    void appl_dumpStats() override;
    bool appl_isDestructible() override;

    // Methods that are not overridden, but use the default implementation:
    // int getDemand(int prevVolume) const override;
    // bool wantsToCommunicate() const override;
    
    // Methods from BaseSatJob:
    bool isInitialized() override;

    void prepareSharing(int maxSize) override;
    bool hasPreparedSharing() override;
    std::vector<int> getPreparedClauses(Checksum& checksum) override;
    void resetLastCommTime() override;

    void digestSharing(std::vector<int>& clauses, const Checksum& checksum) override;
    void returnClauses(std::vector<int>& clauses) override;

private:
    void doStartSolver();

    bool checkClauseComm();
    void loadIncrements();
    void startDestructThreadIfNecessary();

};





#endif
