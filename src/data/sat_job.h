
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

const int RESULT_UNKNOWN = 0;
const int RESULT_SAT = 10;
const int RESULT_UNSAT = 20;

class SatJob : public Job {

private:
    
    std::unique_ptr<HordeLib> solver;
    std::vector<int> clausesToShare;
    int sharedClauseSources = 0;

public:

    SatJob(Parameters& params, int commSize, int worldRank, int jobId, EpochCounter& epochCounter) : 
        Job(params, commSize, worldRank, jobId, epochCounter) {}

    void initialize() override;
    void beginSolving() override;
    void pause() override;
    void unpause() override;
    void terminate() override;
    int solveLoop() override;

    void beginCommunication() override;
    void communicate(int source, JobMessage& msg) override;

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
