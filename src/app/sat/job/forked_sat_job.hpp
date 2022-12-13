
#pragma once

#include <string>
#include <memory>
#include <thread>
#include <atomic>
#include <future>

#include "app/job.hpp"
#include "util/params.hpp"
#include "sat_process_adapter.hpp"
#include "sat_constants.h"
#include "base_sat_job.hpp"

class ForkedSatJob : public BaseSatJob {

private:
    static std::atomic_int _static_subprocess_index;
    
    std::atomic_bool _initialized = false;

    std::unique_ptr<SatProcessAdapter> _solver;
    void* _clause_comm = nullptr; // SatClauseCommunicator instance (avoiding fwd decl.)
    int _last_imported_revision = 0;

    std::future<void> _destruction;
    std::atomic_bool _shmem_freed = false;
    std::list<std::future<void>> _old_solver_destructions;

    float _time_of_start_solving = 0;

    std::atomic_bool _done_locally = false;
    JobResult _internal_result;

    bool _crash_pending {false};

public:

    ForkedSatJob(const Parameters& params, const JobSetup& setup);
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

    // Methods that are not overridden, but use the default implementation:
    // int getDemand(int prevVolume) const override;
    // bool wantsToCommunicate() const override;
    
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

private:
    void doStartSolver();
    void handleSolverCrash();

    bool checkClauseComm();
    void loadIncrements();
    void startDestructThreadIfNecessary();

};
