
#ifndef DOMPASCH_BALANCER_JOB_BASE_H
#define DOMPASCH_BALANCER_JOB_BASE_H

#include <string>
#include <memory>
#include <thread>
#include <initializer_list>
#include <set>

#include "utilities/Threading.h"
#include "util/params.h"
#include "util/permutation.h"
#include "data/job_description.h"
#include "data/job_result.h"
#include "data/job_transfer.h"
#include "data/epoch_counter.h"
#include "data/job_state.h"

class Job {

// Protected fields, may be read and/or manipulated by your application code.
protected:
    /*
    The parameters the application was started with.
    */
    const Parameters& _params;
    /*
    The amount of worker processes (a.k.a. MPI instances, a.k.a. logical "nodes") 
    within this application.
    */
    const int _comm_size;
    /*
    The rank of this process (a.k.a. MPI instance, a.k.a. logical "node") 
    within the set of all processes.
    */
    const int _world_rank;
    /*
    The application writes the result that was found during the solving process
    of this job instance into this field.
    */
    JobResult _result;
    
public:
    // BEGIN of interface to implement as an application.

    /*
    Initialize the solving engine for the job, including initializing 
    and starting the actual solver threads.
    Called inside a thread separated from the main thread,
    so thread safety must be guaranteed w.r.t. other method calls.
    Return true if initialization was successful.
    Return false if initialization was cancelled during execution of the method
    by a concurrent call to appl_withdraw().
    */
    virtual bool appl_initialize() = 0;
    /*
    Return true iff the internal solver engine is fully initialized.
    */
    virtual bool appl_doneInitializing() = 0;
    /*
    Callback for the case where the job instance has changed
    its internal index, i.e. from "2nd node of job #5" to "7th node of job #5".
    The application must update its solvers accordingly (in terms of portfolio diversification, 
    job-internal communication, etc.), immediately or whenever possible.
    */
    virtual void appl_updateRole() = 0;
    /*
    The job description was updated to a new revision.
    Start another solving attempt with the updated description.
    (Only needed for incremental solving.)
    */
    virtual void appl_updateDescription(int fromRevision) = 0;
    /*
    Check from the main thread whether any solution was already found.
    Return a result code if applicable, or -1 otherwise.
    If a solution was found, write it into the inherited field Job::_result .
    */
    virtual int appl_solveLoop() = 0;
    /*
    Suspend your internal solver threads, but leave the possibility
    to resume their execution at some later point. 
    */
    virtual void appl_pause() = 0;
    /*
    Unsuspend your internal solver threads, resuming their execution.
    */
    virtual void appl_unpause() = 0;
    /*
    Interrupt your solver threads' current solving attempt, but (only for incremental solving) 
    leave the possibility to restart another "fresh" solving attempt on an updated description
    at some later point.
    */
    virtual void appl_interrupt() = 0;
    /*
    Clean up the job solver instance, i.e., interrupt your solver threads if they are still running
    and free any memory going beyond meta data.
    If appl_initialize() is running concurrently, this call should also signal it to abort. 
    */
    virtual void appl_withdraw() = 0;
    /*
    Output some statistics about this job instance's solving attempt. 
    This can include CPU utilization per thread,
    performance / done work per thread, total progress if measureable, ...
    */
    virtual void appl_dumpStats() = 0;
    /*
    Return how many processes this job would like to run on based on its meta data 
    and its previous volume.
    This method has a valid default implementation, so it must not be re-implemented.
    It must return an integer greater than 0 and no greater than _comm_size.
    */
    virtual int getDemand(int prevVolume) const;
    /*
    Signal if this job instance would like to initiate a new job communication phase.
    This method has a valid default implementation based on system clock and the "s" arg value, 
    so it must not be re-implemented.
    */
    virtual bool wantsToCommunicate() const;
    /*
    Begin a job communication phase from this node.
    */
    virtual void appl_beginCommunication() = 0;
    /*
    Advance a job communication phase by processing the given message.
    */
    virtual void appl_communicate(int source, JobMessage& msg) = 0;
    /*
    Return true iff the instance can be quickly deleted without any errors
    (i.e., no running concurrent thread of unknown residual life time depends 
    on the instance any more, and if appl_withdraw() contains any heavy lifting 
    then it was already done).
    */
    virtual bool appl_isDestructible() = 0;
    /*
    Free all data associated to this job instance.
    Join all associated threads if any are left.
    */
    virtual ~Job();

    /*
    Measure for the age of a job -- decreases with time.
    Do not reimplement for now.
    */
    virtual double getTemperature() const;
    
    // END of interface to implement as an application.


// Private fields, accessible only by the base implementation in <job.cpp> .
private:
    int _id;
    int _index = -1;
    JobDescription _description;
    std::shared_ptr<std::vector<uint8_t>> _serialized_description;
    std::string _name;
    Mutex _name_change_lock;

    EpochCounter& _epoch_counter;
    int _epoch_of_arrival;
    float _elapsed_seconds_since_arrival;
    float _last_job_comm_remainder = 0;
    float _time_of_initialization = 0;
    float _time_of_abort = 0;

    JobState _state;
    bool _has_description;
    bool _initialized;
    std::unique_ptr<std::thread> _initializer_thread;
    mutable Mutex _job_manipulation_lock;
    
    int _last_volume = 0;
    mutable double _last_temperature = 1.0;
    mutable int _age_of_const_cooldown = -1;
    float _job_comm_period;

    AdjustablePermutation _job_node_ranks;
    bool _has_left_child;
    bool _has_right_child;
    int _client_rank;
    bool _result_transfer_pending = false;

    std::set<int> _past_children;
    std::map<int, int> _dormant_children_num_fails;

// Public methods.
public:

    // Manipulation methods called from outside
    // (NOT to be called by your application code implementing above methods!)

    // Constructor
    Job(Parameters& params, int commSize, int worldRank, int jobId, EpochCounter& epochCounter);
    
    // Equips the job instance with an initial job description in serialized form.
    void setDescription(std::shared_ptr<std::vector<uint8_t>>& data);
    // Adds an amendment to the job description in serialized form.
    void addAmendment(std::shared_ptr<std::vector<uint8_t>>& data);
    
    // Mark the job as being subject of a commitment to the given job request.
    // Requires the job to be not active and not committed.
    void commit(const JobRequest& req);
    // Unmark the job as being subject of a commitment to some job request.
    // Requires the job to be in a committed state.
    void uncommit();
    
    // Marks the job instance to be in an initializing state.
    void beginInitialization();
    // Initializes the job using the meta data it was supplied with during construction.
    // If unsuccessful, autonomously exits the initializing state and withdraws.
    void initialize();
    // Updates the job's meta data before calling initialize().
    void initialize(int index, int rootRank, int parentRank);
    // Return true iff the job instance is in an initializing state, 
    // but internally done initializing.
    bool isDoneInitializing();
    // Unmarks the job instance to be in an initializing state and switches 
    // the job's state according to its secondary state (active, suspended, ...).
    void endInitialization();
    
    // All-in-one method to reactivate a job instance. 
    // If uninitialized, marks the job instance as initializing, updates the meta data, 
    // and begins or continues or reperforms initialization and resuming of solvers 
    // according to the job's internal state.
    void reactivate(int index, int rootRank, int parentRank);

    // Initiate a communication with other nodes in the associated job tree.
    void communicate();
    
    // Freeze the execution of all internal solvers. They can be resumed at any time.
    void suspend();
    // Resume all internal solvers given that they were frozen.
    void resume();
    // Interrupt the execution of all internal solvers. They cannot be resumed.
    void stop();
    // Interrupt the execution of solvers and withdraw the associated solvers 
    // and the job's payload. Only leaves behind the job's meta data.
    void terminate();

    // Lock the job manipulation mutex.
    void lockJobManipulation();
    // Unlock the job manipulation mutex. Requires that it was locked before.
    void unlockJobManipulation();
    

    // Getter methods and simple queries

    // ... for the job state

    // Simple getter for current job state.
    JobState getState() const {return _state;};
    // Acquires job manipulation lock. 
    bool isInState(std::initializer_list<JobState> list) const;
    // Acquires job manipulation lock.
    bool isNotInState(std::initializer_list<JobState> list) const;
    // Does *not* acquire job manipulation lock.
    bool isInStateUnsafe(std::initializer_list<JobState> list) const;
    // Does *not* acquire job manipulation lock.
    bool isNotInStateUnsafe(std::initializer_list<JobState> list) const;
    // Acquires job manipulation lock. 
    bool isInitializing() const {return isInState({INITIALIZING_TO_ACTIVE, INITIALIZING_TO_PAST, INITIALIZING_TO_SUSPENDED, INITIALIZING_TO_COMMITTED});};
    // Does *not* acquire job manipulation lock.
    bool isInitializingUnsafe() const {return isInStateUnsafe({INITIALIZING_TO_ACTIVE, INITIALIZING_TO_PAST, INITIALIZING_TO_SUSPENDED, INITIALIZING_TO_COMMITTED});};
    // Acquires job manipulation lock. 
    bool isActive() const {return isInState({ACTIVE, INITIALIZING_TO_ACTIVE});}
    // Acquires job manipulation lock. 
    bool isCommitted() const {return isInState({COMMITTED, INITIALIZING_TO_COMMITTED});}
    // Acquires job manipulation lock. 
    bool isSuspended() const {return isInState({SUSPENDED, INITIALIZING_TO_SUSPENDED});}
    // Acquires job manipulation lock. 
    bool isPast() const {return isInState({PAST, INITIALIZING_TO_PAST});}
    // Acquires job manipulation lock. 
    bool isForgetting() const {return isInState({FORGETTING});}

    // ... for the job's current meta data

    int getLastVolume() const {return _last_volume;}
    JobDescription& getDescription() {return _description;};
    std::shared_ptr<std::vector<uint8_t>>& getSerializedDescription() {return _serialized_description;};
    int getId() const {return _id;};
    int getIndex() const {return _index;};
    bool isInitialized() const {return _initialized;};
    bool hasJobDescription() const {return _has_description;};
    int getRevision() const {return _description.getRevision();};
    const JobResult& getResult() const;
    // Returns -1 if no job communication is intended. Returns the current job comm epoch (by elapsed seconds) otherwise.
    int getJobCommEpoch() const {return _params.getFloatParam("s") <= 0 ? -1 : (int)(Timer::elapsedSeconds() / _params.getFloatParam("s"));}
    // Elapsed seconds since the job's constructor call.
    float getAge() const {return Timer::elapsedSeconds() - _elapsed_seconds_since_arrival;}
    // Elapsed seconds since initialization was ended.
    float getAgeSinceInitialized() const {return Timer::elapsedSeconds() - _time_of_initialization;}
    // Elapsed seconds since termination of the job.
    float getAgeSinceAbort() const {return Timer::elapsedSeconds() - _time_of_abort;}
    // Return true iff this job instance has found a job result that it still needs to communicate.
    bool isResultTransferPending() {return _result_transfer_pending;}
    // Returns whether the job is easily and quickly destructible as of now. 
    // (calls appl_isDestructible())
    bool isDestructible();

    // ... for the job's affiliated job tree

    bool isRoot() const {return _index == 0;};
    int getRootNodeRank() const {return _job_node_ranks[0];};
    int getLeftChildNodeRank() const {return _job_node_ranks[getLeftChildIndex()];};
    int getRightChildNodeRank() const {return _job_node_ranks[getRightChildIndex()];};
    bool isLeaf() const {return !_has_left_child && !_has_right_child;}
    bool hasLeftChild() const {return _has_left_child;};
    bool hasRightChild() const {return _has_right_child;};
    int getLeftChildIndex() const {return 2*(_index+1)-1;};
    int getRightChildIndex() const {return 2*(_index+1);};
    int getParentNodeRank() const {return isRoot() ? _client_rank : _job_node_ranks[getParentIndex()];};
    int getParentIndex() const {return (_index-1)/2;};
    std::set<int>& getPastChildren() {return _past_children;}
    std::set<int> getDormantChildren() const {
        std::set<int> c;
        for (const auto& entry : _dormant_children_num_fails) {
            c.insert(entry.first);
        }
        return c;
    }
    int getNumFailsOfDormantChild(int rank) const {
        assert(_dormant_children_num_fails.count(rank));
        return _dormant_children_num_fails.at(rank);
    }


    // Setter methods and simple manipulations

    // ... of the job state

    void switchState(JobState state);
    
    // ... of the job's affiliated job tree

    void setLastVolume(int lastVolume) {_last_volume = lastVolume;} // size of tree being aimed at
    void setLeftChild(int rank);
    void setRightChild(int rank);
    void unsetLeftChild() {
        int rank = getLeftChildNodeRank();
        if (_has_left_child) {
            _past_children.insert(rank);
            addDormantChild(rank);
            _has_left_child = false;
        }
    }
    void unsetRightChild() {
        int rank = getRightChildNodeRank();
        if (_has_right_child) {
            _past_children.insert(rank); 
            addDormantChild(rank);
            _has_right_child = false;
        }
    }
    void updateJobNode(int index, int newRank) {_job_node_ranks.adjust(index, newRank);}
    void updateParentNodeRank(int newRank) {
        if (isRoot()) {
            // Root worker node!
            _client_rank = newRank;
        } else {
            // Inner node / leaf worker
            updateJobNode(getParentIndex(), newRank);
        }
    }
    void addDormantChild(int rank) {
        _dormant_children_num_fails[rank] = 0;
    }
    void addFailToDormantChild(int rank) {
        if (!_dormant_children_num_fails.count(rank)) return;
        _dormant_children_num_fails[rank]++;
        if (_dormant_children_num_fails[rank] >= 3)
            _dormant_children_num_fails.erase(rank);
    }

    // ... of various meta data

    void setResultTransferPending(bool pending) {_result_transfer_pending = pending;}
    void setForgetting();


    // toString methods

    const char* toStr() {
        _name_change_lock.lock();
        _name = "#" + std::to_string(_id) + ":" + (_index >= 0 ? std::to_string(_index) : std::string("?"));
        _name_change_lock.unlock();
        return _name.c_str();
    };
    const char* jobStateToStr() const {return jobStateStrings[(int)_state];};
};

#endif