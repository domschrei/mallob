
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
    INITIALIZING_TO_COMMITTED,
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
    PAST,
    /*
     * The job has been finished and is waiting for a directive from its parent
     * and/or the external client.
     */
    STANDBY
};
static const char * jobStateStrings[] = { "none", "stored", "committed", "initializingToActive", 
    "initializingToSuspended", "initializingToPast", "initializingToCommitted", "active", "suspended", "past", "standby" };

class Job {

protected:
    Parameters& _params;
    int _comm_size;
    int _world_rank;

    int _id;
    int _index;
    JobDescription _description;
    std::shared_ptr<std::vector<uint8_t>> _serialized_description;

    EpochCounter& _epoch_counter;
    int _epoch_of_arrival;
    float _elapsed_seconds_since_arrival;
    float _last_job_comm_remainder = 0;
    float _time_of_initialization = 0;

    JobState _state;
    bool _has_description;
    bool _initialized;
    bool _abort_after_initialization;
    std::unique_ptr<std::thread> _initializer_thread;
    bool _done_locally;
    
    int _result_code;
    JobResult _result;
    
    AdjustablePermutation _job_node_ranks;
    bool _has_left_child;
    bool _has_right_child;
    int _client_rank;

public:

    Job(Parameters& params, int commSize, int worldRank, int jobId, EpochCounter& epochCounter);
    void store(std::shared_ptr<std::vector<uint8_t>>& data);
    void setDescription(std::shared_ptr<std::vector<uint8_t>>& data);
    void addAmendment(std::shared_ptr<std::vector<uint8_t>>& data);
    void commit(const JobRequest& req);
    void uncommit(const JobRequest& req);
    void initialize(int index, int rootRank, int parentRank);
    void reinitialize(int index, int rootRank, int parentRank);
    void suspend();
    void resume();
    void stop();
    void terminate();

    // Control methods (must be implemented)
    virtual void updateRole() = 0;
    virtual void updateDescription(int fromRevision) = 0;
    virtual int solveLoop() = 0;
    virtual void pause() = 0;
    virtual void unpause() = 0;
    virtual void interrupt() = 0;
    virtual void withdraw() = 0;
    virtual void dumpStats() = 0;
    // Intra-job communication methods (must be implemented)
    virtual void beginCommunication() = 0;
    virtual void communicate(int source, JobMessage& msg) = 0;

    virtual void initialize();
    void beginInitialization();
    void endInitialization();
    
    // Querying and communication methods (may be re-implemented partially)
    virtual int getDemand(int prevVolume) const;
    virtual bool wantsToCommunicate() const;
    void communicate();

    JobState getState() const {return _state;};
    bool isInState(std::initializer_list<JobState> list) const;
    bool isNotInState(std::initializer_list<JobState> list) const;
    JobDescription& getDescription() {return _description;};
    std::shared_ptr<std::vector<uint8_t>>& getSerializedDescription() {return _serialized_description;};
    int getId() const {return _id;};
    int getIndex() const {return _index;};
    bool isInitialized() const {return _initialized;};
    bool isInitializing() const {return isInState({INITIALIZING_TO_ACTIVE, INITIALIZING_TO_PAST, INITIALIZING_TO_SUSPENDED, INITIALIZING_TO_COMMITTED});};
    bool hasJobDescription() const {return _has_description;};
    int getRevision() const {return _description.getRevision();};

    bool isRoot() const {return _index == 0;};
    int getRootNodeRank() const {return _job_node_ranks[0];};
    int getParentNodeRank() const {return isRoot() ? _client_rank : _job_node_ranks[getParentIndex()];};
    int getLeftChildNodeRank() const {return _job_node_ranks[getLeftChildIndex()];};
    int getRightChildNodeRank() const {return _job_node_ranks[getRightChildIndex()];};

    bool hasLeftChild() const {return _has_left_child;};
    bool hasRightChild() const {return _has_right_child;};
    void setLeftChild(int rank);
    void setRightChild(int rank);
    void unsetLeftChild() {_has_left_child = false;};
    void unsetRightChild() {_has_right_child = false;};

    int getLeftChildIndex() const {return 2*(_index+1)-1;};
    int getRightChildIndex() const {return 2*(_index+1);};
    int getParentIndex() const {return (_index-1)/2;};
    const JobResult& getResult() const;

    const char* toStr() const {return ("#" + std::to_string(_id) + ":" + (_index >= 0 ? std::to_string(_index) : std::string("?"))).c_str();};
    const char* jobStateToStr() const {return jobStateStrings[(int)_state];};

    void updateJobNode(int index, int newRank) {
        _job_node_ranks.adjust(index, newRank);
    }
    void updateParentNodeRank(int newRank) {
        if (isRoot()) {
            // Root worker node!
            _client_rank = newRank;
        } else {
            // Inner node / leaf worker
            updateJobNode(getParentIndex(), newRank);
        }
    }
    void switchState(JobState state);
};

#endif