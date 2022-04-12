
#pragma once

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

class SatEngine; // forward declaration

class ThreadedSatJob : public BaseSatJob {

private:
    std::atomic_bool _initialized = false;

    std::unique_ptr<SatEngine> _solver;
    void* _clause_comm = NULL; // SatClauseCommunicator instance (avoiding fwd decl.)
    std::vector<int> _clause_buffer;
    Checksum _clause_checksum;
    bool _did_filter = false;
    std::vector<int> _filter;
    std::vector<int> _clauses_to_filter;

    std::atomic_bool _done_locally;
    int _result_code;
    JobResult _result;
    int _last_imported_revision = -1;

    std::future<void> _destroy_future;
    Mutex _solver_lock;

    float _time_of_start_solving = 0;

public:

    ThreadedSatJob(const Parameters& params, int commSize, int worldRank, int jobId, JobDescription::Application appl);
    virtual ~ThreadedSatJob() override;

    void appl_start() override;
    void appl_suspend() override;
    void appl_resume() override;
    void appl_terminate() override;

    int appl_solved() override;
    JobResult&& appl_getResult() override;

    void appl_communicate() override;
    void appl_communicate(int source, int mpiTag, JobMessage& msg) override;

    void appl_dumpStats() override;
    bool appl_isDestructible() override;
    void appl_memoryPanic() override;

    // Methods that are not overridden, but use the default implementation:
    // int getDemand(int prevVolume) const override;
    // bool wantsToCommunicate() const override;

    void terminateUnsafe();

    // Methods from BaseSatJob:
    bool isInitialized() override;
    void prepareSharing(int maxSize) override;
    bool hasPreparedSharing() override;
    std::vector<int> getPreparedClauses(Checksum& checksum) override;
    std::pair<int, int> getLastAdmittedClauseShare() override;
    
    virtual void filterSharing(std::vector<int>& clauses) override;
    virtual bool hasFilteredSharing() override;
    virtual std::vector<int> getLocalFilter() override;
    virtual void applyFilter(std::vector<int>& filter) override;

    virtual void digestSharingWithoutFilter(std::vector<int>& clauses) override;
    void returnClauses(std::vector<int>& clauses) override;

    std::unique_ptr<SatEngine>& getSolver() {
        assert(_solver != NULL);
        return _solver;
    }

private:
    void extractResult(int resultCode);
};
