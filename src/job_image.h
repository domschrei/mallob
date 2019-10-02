
#ifndef DOMPASCH_JOB_IMAGE_H
#define DOMPASCH_JOB_IMAGE_H

#include <string>
#include <memory>

#include "HordeLib.h"

#include "job.h"
#include "job_transfer.h"
#include "permutation.h"

/**
 * Internal state of the job's image on this node.
 */
enum JobState {
    /**
     * The job is known for some reason (e.g. a failed commitment),
     * but no job description is known and the job has never been launched.
     */
    NONE,
    /**
     * The job description is known, but the job has never been launched.
     */
    STORED,
    /**
     * A commitment has been made to compute on the job as the node of a certain index,
     * but the job description is not necessarily known yet.
     * The job may also have been started before.
     */
    COMMITTED,
    /**
     * There are threads actively computing on the job.
     */
    ACTIVE,
    /**
     * The threads that once computed on the job are suspended.
     * They may or may not be resumed at a later point.
     */
    SUSPENDED,
    /**
     * The job has been finished or terminated in some sense.
     */
    PAST
};
static const char * jobStateStrings[] = { "none", "stored", "committed", "active", "suspended", "past" };

const int RESULT_UNKNOWN = 0;
const int RESULT_SAT = 10;
const int RESULT_UNSAT = 20;

class JobImage {

private:
    int commSize;
    int worldRank;

    int jobId;
    int index;
    Job job;
    JobState state = JobState::NONE;
    bool hasDescription;
    bool initialized;

    AdjustablePermutation jobNodeRanks;
    bool leftChild = false;
    bool rightChild = false;

    std::unique_ptr<HordeLib> solver;
    bool doneLocally = false;

    std::vector<int> clausesToShare;
    int sharedClauseSources = 0;

public:

    JobImage(int commSize, int worldRank, int jobId);
    void store(Job job);
    void commit(const JobRequest& req);
    void uncommit(const JobRequest& req);
    void initialize(int index, int rootRank, int parentRank);
    void initialize();
    void reinitialize(int index, int rootRank, int parentRank);

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

    bool canShareCollectedClauses();

    /**
     * Give a collection of learned clauses that came from a parent node
     * to the solvers.
     */
    void learnClausesFromAbove(std::vector<int>& clauses);

    void suspend();
    void resume();
    void withdraw();
    int solveLoop();

    JobState getState() const {return state;};
    const Job& getJob() const {return job;};
    Job& getJob() {return job;};
    int getIndex() const {return index;};

    bool isRoot() const {return index == 0;};
    int getRootNodeRank() const {return jobNodeRanks[0];};
    int getParentNodeRank() const {return jobNodeRanks[getParentIndex()];};
    int getLeftChildNodeRank() const {return jobNodeRanks[getLeftChildIndex()];};
    int getRightChildNodeRank() const {return jobNodeRanks[getRightChildIndex()];};

    bool hasLeftChild() const {return leftChild;};
    bool hasRightChild() const {return rightChild;};
    void setLeftChild(int rank);
    void setRightChild(int rank);
    void unsetLeftChild() {leftChild = false;};
    void unsetRightChild() {rightChild = false;};

    int getLeftChildIndex() const {return 2*(index+1)-1;};
    int getRightChildIndex() const {return 2*(index+1);};
    int getParentIndex() const {return (index-1)/2;};

    std::string toStr() const {return "#" + std::to_string(jobId) + ":" + std::to_string(index);};
    std::string jobStateToStr() const {return std::string(jobStateStrings[(int)state]);};

    void updateJobNode(int index, int newRank) {
        jobNodeRanks.adjust(index, newRank);
    }

private:
    void insertIntoClauseBuffer(std::vector<int>& vec);
};





#endif
