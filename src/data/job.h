
#ifndef DOMPASCH_BALANCER_JOB_BASE_H
#define DOMPASCH_BALANCER_JOB_BASE_H

#include <string>
#include <memory>
#include <thread>
#include <initializer_list>

#include "util/params.h"
#include "util/permutation.h"
#include "data/job_description.h"
#include "data/job_transfer.h"
#include "data/epoch_counter.h"

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
     * The job is currently being initialized (by a separate thread).
     */
    INITIALIZING_TO_ACTIVE,
    INITIALIZING_TO_SUSPENDED,
    INITIALIZING_TO_PAST,
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
static const char * jobStateStrings[] = { "none", "stored", "committed", "initializingToActive", "initializingToSuspended", "initializingToPast", "active", "suspended", "past" };

class Job {

protected:
    Parameters& params;
    int commSize;
    int worldRank;

    int jobId;
    int index;
    JobDescription job;
    std::vector<int> serializedDescription;

    EpochCounter& epochCounter;
    int epochOfArrival;
    float elapsedSecondsOfArrival;
    int epochOfLastCommunication = -1; 

    JobState state = JobState::NONE;
    bool hasDescription;
    bool initialized;
    std::unique_ptr<std::thread> initializerThread;
    bool doneLocally = false;
    
    int resultCode;
    JobResult result;
    
    AdjustablePermutation jobNodeRanks;
    bool leftChild = false;
    bool rightChild = false;
    int clientRank;

public:

    Job(Parameters& params, int commSize, int worldRank, int jobId, EpochCounter& epochCounter);
    void store(std::vector<int>& data);
    void setDescription(std::vector<int>& data);
    void commit(const JobRequest& req);
    void uncommit(const JobRequest& req);
    void initialize(int index, int rootRank, int parentRank);
    void reinitialize(int index, int rootRank, int parentRank);
    void suspend();
    void resume();
    void withdraw();

    // Control methods (must be implemented)
    virtual void beginSolving() = 0;
    virtual int solveLoop() = 0;
    virtual void pause() = 0;
    virtual void unpause() = 0;
    virtual void terminate() = 0;

    virtual void initialize();
    void beginInitialization();
    void endInitialization();
    
    // Intra-job communication methods (must be implemented)
    virtual void beginCommunication() = 0;
    virtual void communicate(int source, JobMessage& msg) = 0;
    
    // Querying and communication methods (may be re-implemented partially)
    virtual int getDemand() const;
    virtual bool wantsToCommunicate() const;
    void communicate();


    JobState getState() const {return state;};
    bool isInState(std::initializer_list<JobState> list) const;
    bool isNotInState(std::initializer_list<JobState> list) const;
    JobDescription& getDescription() {return job;};
    std::vector<int>& getSerializedDescription() {return serializedDescription;};
    int getIndex() const {return index;};
    bool isInitialized() const {return initialized;};

    bool isRoot() const {return index == 0;};
    int getRootNodeRank() const {return jobNodeRanks[0];};
    int getParentNodeRank() const {return isRoot() ? clientRank : jobNodeRanks[getParentIndex()];};
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
    const JobResult& getResult() const;

    const char* toStr() const {return ("#" + std::to_string(jobId) + ":" + (index >= 0 ? std::to_string(index) : std::string("?"))).c_str();};
    const char* jobStateToStr() const {return jobStateStrings[(int)state];};

    void updateJobNode(int index, int newRank) {
        jobNodeRanks.adjust(index, newRank);
    }
    void updateParentNodeRank(int newRank) {
        if (isRoot()) {
            // Root worker node!
            clientRank = newRank;
        } else {
            // Inner node / leaf worker
            updateJobNode(getParentIndex(), newRank);
        }
    }
    void switchState(JobState state);
};

#endif