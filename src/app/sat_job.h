
#ifndef DOMPASCH_JOB_IMAGE_H
#define DOMPASCH_JOB_IMAGE_H

#include <string>
#include <memory>
#include <thread>

#include "HordeLib.h"

#include "data/job.h"
#include "util/params.h"
#include "util/permutation.h"
#include "data/job_description.h"
#include "data/job_transfer.h"
#include "data/epoch_counter.h"

const int RESULT_UNKNOWN = 0;
const int RESULT_SAT = 10;
const int RESULT_UNSAT = 20;

class SatJob : public Job {

private:
    bool _abort_after_initialization = false;

    std::unique_ptr<HordeLib> _solver;
    void* _clause_comm = NULL; // SatClauseCommunicator instance (avoiding fwd decl.)

    bool _done_locally;

    std::thread _bg_thread;
    bool _bg_thread_running;
    Mutex _horde_manipulation_lock;

    float _time_of_start_solving = 0;

public:

    SatJob(Parameters& params, int commSize, int worldRank, int jobId, EpochCounter& epochCounter);
    ~SatJob() override;

    bool appl_initialize() override;
    void appl_updateRole() override;
    void appl_updateDescription(int fromRevision) override;
    void appl_pause() override;
    void appl_unpause() override;
    void appl_interrupt() override;
    void appl_withdraw() override;
    int appl_solveLoop() override;

    void appl_beginCommunication() override;
    void appl_communicate(int source, JobMessage& msg) override;

    void appl_dumpStats() override;
    bool appl_isDestructible() override;

    // Methods that are not overridden, but use the default implementation:
    // int getDemand(int prevVolume) const override;
    // bool wantsToCommunicate() const override;

    std::unique_ptr<HordeLib>& getSolver() {return _solver;}

    void lockHordeManipulation();
    void unlockHordeManipulation();

private:
    void extractResult(int resultCode);
    void setSolverNullThread();
    void setSolverNull();
};





#endif
