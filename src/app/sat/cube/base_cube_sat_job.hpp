#ifndef MSCHICK_BASE_CUBE_SAT_JOB_H
#define MSCHICK_BASE_CUBE_SAT_JOB_H

#include "cube_lib.hpp"

#include "app/sat/base_sat_job.hpp"

class BaseCubeSatJob : public BaseSatJob {

private:
    std::unique_ptr<CubeLib> _lib;

    void* _clause_comm = NULL; // SatClauseCommunicator instance (avoiding fwd decl.)

    void* _cube_comm = NULL; // CubeCommunicator instance (avoiding fwd decl.)

    float _time_of_start_solving = 0;
    float _time_of_last_comm = 0;
    float _job_comm_period;

    bool _done_locally = false;

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

    // Methods from BaseSatJob:
    bool isInitialized() override;
    void prepareSharing(int maxSize) override;
    bool hasPreparedSharing() override;
    std::vector<int> getPreparedClauses() override;
    void digestSharing(const std::vector<int>& clauses) override;

    // Methods from BaseCubeSatJob:
    void prepareCubes();
    bool hasPreparedCubes();
    std::vector<int> getPreparedCubes();
    void digestCubes(const std::vector<int>& cubes);

private:
    std::unique_ptr<CubeLib>& getLib() {
        assert(libNotNull());
        return _lib;
    }

    bool libNotNull() {
        return _lib != NULL;
    }
};

#endif /* MSCHICK_BASE_CUBE_SAT_JOB_H */