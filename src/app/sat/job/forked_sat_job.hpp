
#pragma once

#include <string>
#include <memory>
#include <thread>
#include <atomic>
#include <future>
#include <list>
#include <vector>

#include "app/app_message_subscription.hpp"
#include "app/job.hpp"
#include "util/sys/shmem_cache.hpp"
#include "util/params.hpp"
#include "sat_process_adapter.hpp"
#include "sat_constants.h"
#include "base_sat_job.hpp"
#include "data/job_result.hpp"

class AnytimeSatClauseCommunicator; // fwd decl
class Checksum;
class Parameters;
class SatProcessAdapter;
struct JobMessage;

class ForkedSatJob : public BaseSatJob {

private:
    static std::atomic_int _static_subprocess_index;
    
    std::atomic_bool _initialized = false;
    int _cores_allocated {0};

    std::unique_ptr<SatProcessAdapter> _solver;
    int _last_imported_revision = 0;
    std::vector<SatProcessAdapter::ShmemObject> _formulas_in_shmem;

    std::future<void> _destruction;
    std::atomic_bool _shmem_freed = false;
    std::list<std::future<void>> _old_solver_destructions;

    float _time_of_start_solving = 0;

    std::atomic_bool _done_locally = false;
    bool _assembling_proof = false;
    JobResult _internal_result;

    int _sharing_max_size {0};

    int _subproc_idx;

    float _time_of_retraction_start = -1;
    float _time_of_retraction_end = -1;
    float _retraction_round_duration = 0;

public:

    ForkedSatJob(const Parameters& params, const JobSetup& setup, AppMessageTable& table);
    virtual ~ForkedSatJob() override;

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

    int getDemand() const override;

    // Methods that are not overridden, but use the default implementation:
    // bool wantsToCommunicate() const override;
    
    // Methods from BaseSatJob:
    bool isInitialized() override;

    void prepareSharing() override;
    bool hasPreparedSharing() override;
    std::vector<int> getPreparedClauses(Checksum& checksum, int& successfulSolverId, int& numLits) override;
    int getLastAdmittedNumLits() override;
    long long getBestFoundObjectiveCost() override;
    virtual void setClauseBufferRevision(int revision) override;
    virtual void updateBestFoundSolutionCost(long long bestFoundSolutionCost) override;

    virtual void filterSharing(int epoch, std::vector<int>&& clauses) override;
    virtual bool hasFilteredSharing(int epoch) override;
    virtual std::vector<int> getLocalFilter(int epoch) override;
    virtual void applyFilter(int epoch, std::vector<int>&& filter) override;
    virtual void digestSharingWithoutFilter(int epoch, std::vector<int>&& clauses, bool stateless) override;
    
    virtual void returnClauses(std::vector<int>&& clauses) override;
    virtual void digestHistoricClauses(int epochBegin, int epochEnd, std::vector<int>&& clauses) override;

    virtual bool canHandleIncompleteRevision(int rev) override;

private:
    void doStartSolver();
    void handleSolverCrash();

    void loadIncrements();
    void startDestructThreadIfNecessary();

};
