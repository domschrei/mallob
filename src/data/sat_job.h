
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

#define BROADCAST_CLAUSE_INTS_PER_NODE (MAX_JOB_MESSAGE_PAYLOAD_PER_NODE/sizeof(int))

const int MSG_GATHER_CLAUSES = 417;
const int MSG_DISTRIBUTE_CLAUSES = 418;

const int RESULT_UNKNOWN = 0;
const int RESULT_SAT = 10;
const int RESULT_UNSAT = 20;

class SatJob : public Job {

private:
    
    std::unique_ptr<HordeLib> _solver;
    std::vector<int> _clause_buffer;
    int _num_clause_sources;
    int _job_comm_epoch_of_clause_buffer;
    int _last_shared_job_comm;

    std::thread bgThread;
    Mutex hordeManipulationLock;

public:

    SatJob(Parameters& params, int commSize, int worldRank, int jobId, EpochCounter& epochCounter) : 
        Job(params, commSize, worldRank, jobId, epochCounter), _num_clause_sources(0), 
        _job_comm_epoch_of_clause_buffer(-1), _last_shared_job_comm(-1) {}
    ~SatJob() override;

    void appl_initialize() override;
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

private:
    void extractResult();

    void setSolverNull();

    bool canShareCollectedClauses();
    void learnAndDistributeClausesDownwards(std::vector<int>& clauses, int jobCommEpoch);
    /**
     * Get clauses to share from the solvers, in order to propagate it to the parents.
     */
    std::vector<int> collectClausesFromSolvers();
    /**
     * Store clauses from a child node in order to propagate it upwards later.
     */
    void collectClausesFromBelow(std::vector<int>& clauses, int jobCommEpoch);
    /**
     * Returns all clauses that have been added by addClausesFromBelow(·),
     * plus the clauses from an additional call to collectClausesToShare(·).
     */
    std::vector<int> shareCollectedClauses(int jobCommEpoch);
    /**
     * Give a collection of learned clauses that came from a parent node
     * to the solvers.
     */
    void learnClausesFromAbove(std::vector<int>& clauses, int jobCommEpoch);
    void insertIntoClauseBuffer(std::vector<int>& vec, int jobCommEpoch);
};





#endif
