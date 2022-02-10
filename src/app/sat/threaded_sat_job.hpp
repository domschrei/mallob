
#ifndef DOMPASCH_JOB_IMAGE_H
#define DOMPASCH_JOB_IMAGE_H

#include <string>
#include <memory>
#include <thread>
#include <future>
#include "util/assert.hpp"

#include "app/job.hpp"
#include "util/params.hpp"
#include "util/permutation.hpp"
#include "data/job_description.hpp"
#include "data/job_transfer.hpp"
#include "sat_constants.h"
#include "base_sat_job.hpp"

class HordeLib; // forward declaration

class ThreadedSatJob : public BaseSatJob {

private:
    std::atomic_bool _initialized = false;

    std::unique_ptr<HordeLib> _solver;
    void* _clause_comm = NULL; // SatClauseCommunicator instance (avoiding fwd decl.)
    std::vector<int> _clause_buffer;
    Checksum _clause_checksum;

    std::atomic_bool _done_locally;
    int _result_code;
    JobResult _result;
    int _last_imported_revision = -1;

    std::future<void> _destroy_future;
    Mutex _solver_lock;

    float _time_of_start_solving = 0;
    float _time_of_last_comm = 0;
    float _job_comm_period;

public:

    ThreadedSatJob(const Parameters& params, int commSize, int worldRank, int jobId, JobDescription::Application appl);
    virtual ~ThreadedSatJob() override;

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

    void terminateUnsafe();

    // Methods from BaseSatJob:
    bool isInitialized() override;
    void prepareSharing(int maxSize) override;
    bool hasPreparedSharing() override;
    std::vector<int> getPreparedClauses(Checksum& checksum) override;
    void resetLastCommTime() override;
    
    void digestSharing(std::vector<int>& clauses, const Checksum& checksum) override;
    void returnClauses(std::vector<int>& clauses) override;

    std::unique_ptr<HordeLib>& getSolver() {
        assert(_solver != NULL);
        return _solver;
    }

private:
    void extractResult(int resultCode);
};





#endif
