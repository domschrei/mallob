
#ifndef DOMPASCH_BALANCER_JOB_BASE_H
#define DOMPASCH_BALANCER_JOB_BASE_H

#include <string>
#include <memory>
#include "util/assert.hpp"
#include <atomic>
#include <list>

#include "util/sys/threading.hpp"
#include "util/params.hpp"
#include "data/job_description.hpp"
#include "data/job_result.hpp"
#include "data/job_transfer.hpp"
#include "data/job_state.h"
#include "util/logger.hpp"
#include "app/job_tree.hpp"
#include "comm/job_comm.hpp"
#include "scheduling/local_scheduler.hpp"

typedef std::function<void(JobRequest& req, int tag, bool left, int dest)> EmitDirectedJobRequestCallback;

class Job {

// Protected fields, may be accessed by your application code.
protected:
    /*
    The parameters the application was started with.
    */
    const Parameters& _params;
    
public:
    // BEGIN of interface to implement as an application.

    /*
    State diagram:

        |
        v                         <--resume()---
     INACTIVE --start()--> ACTIVE --suspend()--> SUSPENDED
        |                    |                      |
        ----------------terminate()------------------
                             |
                             v
                            PAST

    General notes:
    * Communication (MyMpi::*) may ONLY take place within mallob's main thread, i.e., 
      within the appl_* methods (and NOT within a thread spawned from it).
    * All appl_* methods must return very quickly. Heavy work must be performed concurrently
      within a separate thread.
    * Your code must bookkeep the active threads of your application, make them ready to be joined 
      after appl_terminate() has been called, and be able to instantly join them as soon as
      your appl_isDestructible() returns true. 
    * If your application spawns child processes, appl_suspend() and appl_resume() can be
      realized over SIGTSTP and SIGCONT signals, but do make sure that the process is
      terminated after appl_terminate().
    */

    /*
    Begin, or continue, to process the job.
    At the first call of appl_start(), you can safely assume that the job description
    is already present: getDescription() returns a valid JobDescription instance.
    */
    virtual void appl_start() = 0;
    /*
    Suspend the job, enabling a resumption of the job processing at a later time.
    After suspension is completed, the job should not spend any CPU time on itself any more.
    */
    virtual void appl_suspend() = 0;
    /*
    Resume a suspended job.
    */
    virtual void appl_resume() = 0;
    /*
    Terminate a job which is irrecoverably marked for deletion: This method
    can concurrently trigger a cleanup of any resources needed for solving the job.
    */
    virtual void appl_terminate() = 0;
    /*
    Check from the main thread whether any solution was already found.
    Return a result code if applicable, or -1 otherwise.
    This method can be used to perform further (quick) checks regarding the application
    and can be expected to be called frequently while the job is active.
    For incremental applications, use this method to import new increments
    of getDescription() to your application.
    */
    virtual int appl_solved() = 0;
    /*
    appl_solved() returned a result >= 0; return a fitting JobResult instance at this point.
    This method must only be valid once after a solution has been found.
    */
    virtual JobResult&& appl_getResult() = 0;
    /*
    Perform job-internal communication from this node.
    */
    virtual void appl_communicate() = 0;
    /*
    Advance a job communication phase by processing the given message.
    */
    virtual void appl_communicate(int source, int mpiTag, JobMessage& msg) = 0;
    /*
    Output some statistics about this job instance's solving attempt. 
    This can include CPU utilization per thread,
    performance / done work per thread, total progress if measureable, ...
    */
    virtual void appl_dumpStats() = 0;
    /*
    Return true iff the instance can be quickly deleted without any errors
    (i.e., no running concurrent thread of unknown residual life time depends 
    on the instance any more, and the destructor will return immediately).
    */
    virtual bool appl_isDestructible() = 0;
    /*
    Perform measures to immediately reduce memory usage as far as possible,
    even if it reduces the performance of local job processing significantly.
    */
    virtual void appl_memoryPanic() = 0;
    /*
    Return how many processes this job would like to run on based on its meta data 
    and its previous volume.
    This method must return an integer greater than 0 and no greater than _comm_size. 
    It has a valid default implementation, so it does not need to be re-implemented.
    */
    virtual int getDemand() const;

    /*
    Measure for the age of a job -- decreases with time.
    Do not reimplement for now.
    */
    virtual double getTemperature() const;
    
    /*
    Free all data associated to this job instance. Join all associated threads if any are left.
    */
    virtual ~Job() {}
    // END of interface to implement as an application.


// Private fields, accessible only by the base implementation in <job.cpp> .
private:
    int _id;
    std::string _name;
    int _application_id;
    bool _incremental;

    std::atomic_bool _has_description = false;
    JobDescription _description;
    int _desired_revision = 0;
    int _last_solved_revision = -1;

    float _time_of_arrival;
    float _time_of_activation = 0;
    float _time_of_first_volume_update = -1;
    float _time_of_abort = 0;
    
    float _time_of_last_comm = 0;
    float _time_of_last_limit_check = 0;
    float _time_of_last_ranklist_agg = 0;
    float _used_cpu_seconds = 0;
    
    float _growth_period;
    bool _continuous_growth;
    int _max_demand;
    int _threads_per_job;
    
    JobState _state;
    Mutex _job_manipulation_lock;
    std::optional<JobRequest> _commitment;
    int _balancing_epoch_of_last_commitment = -1;
    std::optional<JobResult> _result;
    robin_hood::unordered_node_set<std::pair<int, int>, IntPairHasher> _waiting_rank_revision_pairs;

    JobTree _job_tree;
    JobComm _comm;
    
    int _volume = 1;
    float _priority = 0.01;
    mutable double _last_temperature = 1.0;
    mutable int _age_of_const_cooldown = -1;
    mutable int _last_demand = 0;

    std::optional<JobRequest> _request_to_multiply_left;
    std::optional<JobRequest> _request_to_multiply_right;

// Public methods.
public:

    // Manipulation methods called from outside
    // (NOT to be called by your application code implementing above methods!)

    // Constructor
    struct JobSetup {
        int commSize;
        int worldRank;
        int jobId;
        int applicationId;
        bool incremental;
    };
    Job(const Parameters& params, const JobSetup& setup);
    
    // Mark the job as being subject of a commitment to the given job request.
    // Requires the job to be not active and not committed.
    void commit(const JobRequest& req);
    // Unmark the job as being subject of a commitment to some job request.
    // Requires the job to be in a committed state.
    std::optional<JobRequest> uncommit();
    // Add the job description of the next (or the first/only) revision.
    void pushRevision(const std::shared_ptr<std::vector<uint8_t>>& data);
    // Starts the execution of a new job.
    void start();
    // Suspend the execution of all internal solvers. They can be resumed at any time.
    void suspend();
    // Resume all internal solvers given that they were suspended.
    void resume();

    // Initiate a communication with other nodes in the associated job tree.
    void communicate(); // outgoing
    void communicate(int source, int mpiTag, JobMessage& msg); // incoming (+ outgoing)

    // Interrupt the execution of solvers and withdraw the associated solvers 
    // and the job's payload. Only leaves behind the job's meta data.
    void terminate();

    // Updates the role of this job instance within the distributed job tree.
    // This method marks this job instance as the <index>th node working on it
    // and remembers the ranks of the root node of the job and of the parent
    // of this node.
    void updateJobTree(int index, int rootRank, int parentRank);


    // Getter methods and simple queries

    JobState getState() const {return _state;};
    void assertState(JobState state) const {assert(_state == state || LOG_RETURN_FALSE("State of %s : %s\n", toStr(), jobStateToStr()));};
    int getVolume() const {return _volume;}
    float getPriority() const {return _priority;}
    int getApplicationId() const {return _application_id;}
    bool isIncremental() const {return _incremental;}
    bool hasDescription() const {return _has_description;};
    const JobDescription& getDescription() const {assert(hasDescription()); return _description;};
    const std::shared_ptr<std::vector<uint8_t>>& getSerializedDescription(int revision) {return _description.getSerialization(revision);};
    bool hasCommitment() const {return _commitment.has_value();}
    const JobRequest& getCommitment() const {assert(hasCommitment()); return _commitment.value();}
    int getId() const {return _id;};
    int getIndex() const {return _job_tree.getIndex();};
    int getRevision() const {return !hasDescription() ? -1 : getDescription().getRevision();};
    int getMaxConsecutiveRevision() const {return !hasDescription() ? -1 : getDescription().getMaxConsecutiveRevision();};
    int getDesiredRevision() const {return _desired_revision;}
    JobResult& getResult();
    // Elapsed seconds since the job's constructor call.
    float getAge() const {return Timer::elapsedSeconds() - _time_of_arrival;}
    // Elapsed seconds since initialization was ended.
    float getAgeSinceActivation() const {return Timer::elapsedSeconds() - _time_of_activation;}
    // Elapsed seconds since termination of the job.
    float getAgeSinceAbort() const {return Timer::elapsedSeconds() - _time_of_abort;}
    float getLatencyOfFirstVolumeUpdate() const {return _time_of_first_volume_update < 0 ? -1 : _time_of_first_volume_update - _time_of_activation;}
    float getUsedCpuSeconds() const {return _used_cpu_seconds;}
    int getNumThreads() const {return _threads_per_job;}
    void setNumThreads(int nbThreads) {_threads_per_job = nbThreads;} 
    int getBalancingEpochOfLastCommitment() const {return _balancing_epoch_of_last_commitment;}
    int getLastDemand() const {return _last_demand;}
    void setLastDemand(int demand) {_last_demand = demand;}

    // Returns whether the job is easily and quickly destructible as of now. 
    // (calls appl_isDestructible())
    bool isDestructible();
    void clearJobDescription() {for (size_t i = 0; i < getRevision(); i++) _description.clearPayload(i);}
    void setTimeOfFirstVolumeUpdate(float time) {_time_of_first_volume_update = time;}
    
    int getGlobalNumWorkers() const {return _job_tree.getCommSize();}
    int getMyMpiRank() const {return _job_tree.getRank();}
    JobTree& getJobTree() {return _job_tree;}
    const JobTree& getJobTree() const {return _job_tree;}
    const JobComm& getJobComm() const {return _comm;}
    robin_hood::unordered_node_set<std::pair<int, int>, IntPairHasher>& getWaitingRankRevisionPairs() {
        return _waiting_rank_revision_pairs;
    }

    // Updates the job's resource usage based on the period of time which passed
    // since the last call (or the job's activation) and the old volume of the job,
    // and then updates the volume itself.
    void updateVolumeAndUsedCpu(int newVolume) {

        if (getState() == ACTIVE && _job_tree.isRoot()) {
            // Compute used CPU time within last time slice
            float time = Timer::elapsedSecondsCached();
            _used_cpu_seconds += (time - _time_of_last_limit_check) * _threads_per_job * _volume;
            _time_of_last_limit_check = time;
        }
        
        // Update volume
        _volume = newVolume;
    }

    // Updates the job's resource usage and then checks whether the job reached
    // a limit imposed by its description or by a global parameter.
    bool checkResourceLimit(float wcSecsPerInstance, float cpuSecsPerInstance) {

        updateVolumeAndUsedCpu(getVolume());
        float usedWcSecs = getAgeSinceActivation();
        float usedCpuSecs = getUsedCpuSeconds();

        if ((cpuSecsPerInstance > 0 && usedCpuSecs > cpuSecsPerInstance)
            || (hasDescription() && getDescription().getCpuLimit() > 0 && 
                usedCpuSecs > getDescription().getCpuLimit())) {
            // Job exceeded its cpu time limit
            LOG(V2_INFO, "#%i CPU TIMEOUT: aborting\n", _id);
            return true;
        }

        if ((wcSecsPerInstance > 0 && usedWcSecs > wcSecsPerInstance) 
            || (hasDescription() && getDescription().getWallclockLimit() > 0 && 
                usedWcSecs > getDescription().getWallclockLimit())) {
            // Job exceeded its wall clock time limit
            LOG(V2_INFO, "#%i WALLCLOCK TIMEOUT: aborting\n", _id);
            return true;
        }

        return false;
    }

    // Marks the job to be indestructible as long as pending is true.
    void addChildWaitingForRevision(int rank, int revision) {_waiting_rank_revision_pairs.insert(std::pair<int, int>(rank, revision));}
    void setDesiredRevision(int revision) {_desired_revision = revision;}
    bool isRevisionSolved(int revision) {return _last_solved_revision >= revision;}
    void setRevisionSolved(int revision) {_last_solved_revision = revision;}

    void storeRequestToMultiply(JobRequest&& req, bool left) {
        (left ? _request_to_multiply_left : _request_to_multiply_right) = std::move(req);
    }
    std::optional<JobRequest>& getRequestToMultiply(bool left) {
        return left ? _request_to_multiply_left : _request_to_multiply_right;
    }

    JobRequest spawnJobRequest(bool left, int balancingEpoch) {
        int index = left ? _job_tree.getLeftChildIndex() : _job_tree.getRightChildIndex();
        if (_params.monoFilename.isSet()) _job_tree.updateJobNode(index, index);

        JobRequest req(_id, _application_id, _job_tree.getRootNodeRank(), 
                MyMpi::rank(MPI_COMM_WORLD), index, Timer::elapsedSeconds(), balancingEpoch, 
                0, _incremental);
        req.revision = std::max(0, getDesiredRevision());
        return req;
    }

    // toString methods

    const char* toStr() const {
        return _name.c_str();
    };
    const char* jobStateToStr() const {return JOB_STATE_STRINGS[(int)_state];};
    static std::string toStr(int jobId, int jobIndex) {
        return "#" + std::to_string(jobId) + ":" + std::to_string(jobIndex);
    }
};

#endif