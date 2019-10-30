
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

#define BROADCAST_CLAUSE_INTS_PER_NODE MAX_JOB_MESSAGE_PAYLOAD_PER_NODE

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

public:

    SatJob(Parameters& params, int commSize, int worldRank, int jobId, EpochCounter& epochCounter) : 
        Job(params, commSize, worldRank, jobId, epochCounter), _num_clause_sources(0) {}

    void initialize() override;
    void beginSolving() override;
    void pause() override;
    void unpause() override;
    void terminate() override;
    int solveLoop() override;

    void beginCommunication() override;
    void communicate(int source, JobMessage& msg) override;

    void dumpStats() override;

private:
    void extractResult();

    bool canShareCollectedClauses();
    void learnAndDistributeClausesDownwards(std::vector<int>& clauses);
    /**
     * Get clauses to share from the solvers, in order to propagate it to the parents.
     */
    std::vector<int> collectClausesFromSolvers();
    /**
     * Store clauses from a child node in order to propagate it upwards later.
     */
    void collectClausesFromBelow(std::vector<int>& clauses);
    /**
     * Returns all clauses that have been added by addClausesFromBelow(·),
     * plus the clauses from an additional call to collectClausesToShare(·).
     */
    std::vector<int> shareCollectedClauses();
    /**
     * Give a collection of learned clauses that came from a parent node
     * to the solvers.
     */
    void learnClausesFromAbove(std::vector<int>& clauses);
    void insertIntoClauseBuffer(std::vector<int>& vec);
};





#endif
