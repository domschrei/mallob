
#ifndef DOMPASCH_CHILD_PROCESS_JOB_IMAGE_H
#define DOMPASCH_CHILD_PROCESS_JOB_IMAGE_H

#include <string>
#include <memory>
#include <thread>
#include <assert.h>

#include "HordeLib.h"

#include "data/job.h"
#include "util/params.h"
#include "util/permutation.h"
#include "data/job_description.h"
#include "data/job_transfer.h"
#include "data/epoch_counter.h"
#include "horde_process_adapter.h"
#include "app/sat_constants.h"
#include "app/base_sat_job.h"

class ForkedSatJob : public BaseSatJob {

private:
    volatile bool _abort_after_initialization = false;

    std::unique_ptr<HordeProcessAdapter> _solver;
    void* _clause_comm = NULL; // SatClauseCommunicator instance (avoiding fwd decl.)

    mutable Mutex _solver_lock;

    float _time_of_start_solving = 0;
    float _time_of_last_comm = 0;
    float _job_comm_period;

    bool _done_locally = false;

public:

    ForkedSatJob(Parameters& params, int commSize, int worldRank, int jobId, EpochCounter& epochCounter);
    ~ForkedSatJob() override;

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

    // Methods that are not overridden, but use the default implementation:
    // int getDemand(int prevVolume) const override;
    // bool wantsToCommunicate() const override;
    
    // Methods from BaseSatJob:
    bool isInitialized() override;
    void prepareSharing(int maxSize) override;
    bool hasPreparedSharing() override;
    std::vector<int> getPreparedClauses() override;
    void digestSharing(const std::vector<int>& clauses) override;

private:

    std::unique_ptr<HordeProcessAdapter>& getSolver() {
        assert(_solver != NULL);
        return _solver;
    }

    bool solverNotNull() {
        return _solver != NULL;
    }
};





#endif