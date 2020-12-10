
#ifndef DOMPASCH_BALANCER_JOB_BASE_H
#define DOMPASCH_BALANCER_JOB_BASE_H

#include <string>
#include <memory>
#include <thread>
#include <initializer_list>
#include <set>
#include <assert.h>

#include "util/sys/threading.hpp"
#include "util/params.hpp"
#include "util/permutation.hpp"
#include "data/job_description.hpp"
#include "data/job_result.hpp"
#include "data/job_transfer.hpp"
#include "data/epoch_counter.hpp"
#include "data/job_state.h"
#include "util/console.hpp"
#include "app/job_tree.hpp"

class Job {

// Protected fields, may be read and/or manipulated by your application code.
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
                             v
    PAST <--terminate()-- INACTIVE --start()--> ACTIVE --suspend()--> STANDBY
                                   <--stop()---        <--resume()---

    */

    /*
    Begin, or continue, to process the job. This method must return immediately or almost immediately,
    i.e., any heavy tasks must be done within separate threads or processes.
    appl_start() may be called again after appl_stop(); in this case, the internal job data
    (i.e. its description) may have changed and must be reconsidered.
    */
    virtual void appl_start(std::shared_ptr<std::vector<uint8_t>> data) = 0;
    /*
    Stop to process an active job.
    */
    virtual void appl_stop() = 0;
    /*
    Suspend the job, enabling a resumption of the job processing at a later time.
    */
    virtual void appl_suspend() = 0;
    /*
    Resume a suspended job in the same internal state where it was suspended.
    */
    virtual void appl_resume() = 0;
    /*

    */
    virtual void appl_terminate() = 0;
    /*
    Check from the main thread whether any solution was already found.
    Return a result code if applicable, or -1 otherwise.
    */
    virtual int appl_solved() = 0;
    /*

    */
    virtual JobResult appl_getResult() = 0;
    /*
    Signal if this job instance would like to initiate a new job communication phase.
    This method has a valid default implementation based on system clock and the "s" arg value, 
    so it must not be re-implemented.
    */
    virtual bool appl_wantsToBeginCommunication() = 0;
    /*
    Begin a job communication phase from this node.
    */
    virtual void appl_beginCommunication() = 0;
    /*
    Advance a job communication phase by processing the given message.
    */
    virtual void appl_communicate(int source, JobMessage& msg) = 0;
    /*
    Output some statistics about this job instance's solving attempt. 
    This can include CPU utilization per thread,
    performance / done work per thread, total progress if measureable, ...
    */
    virtual void appl_dumpStats() = 0;
    /*
    Return how many processes this job would like to run on based on its meta data 
    and its previous volume.
    This method has a valid default implementation, so it does not need to be re-implemented.
    It must return an integer greater than 0 and no greater than _comm_size.
    */
    virtual int getDemand(int prevVolume, float elapsedTime = Timer::elapsedSeconds()) const;

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
    virtual ~Job() {}
    
    /*
    Measure for the age of a job -- decreases with time.
    Do not reimplement for now.
    */
    virtual double getTemperature() const;
    
    // END of interface to implement as an application.


// Private fields, accessible only by the base implementation in <job.cpp> .
private:
    int _id;
    std::string _name;
    std::optional<JobDescription> _description;
    std::shared_ptr<std::vector<uint8_t>> _serialized_description;

    std::vector<std::thread> _unpack_threads;
    std::vector<bool> _unpack_done;

    float _time_of_arrival;
    float _time_of_initialization = 0;
    float _time_of_abort = 0;
    float _time_of_last_comm = 0;
    float _time_of_last_limit_check = 0;
    float _used_cpu_seconds = 0;
    
    float _growth_period;
    bool _continuous_growth;
    int _max_demand;
    int _threads_per_job;
    
    JobState _state;
    Mutex _job_manipulation_lock;
    std::optional<JobRequest> _commitment;
    std::optional<JobResult> _result;
    bool _result_transfer_pending = false;

    JobTree _job_tree;
    
    int _volume = 1;
    float _priority = 0.01;
    mutable double _last_temperature = 1.0;
    mutable int _age_of_const_cooldown = -1;

// Public methods.
public:

    // Manipulation methods called from outside
    // (NOT to be called by your application code implementing above methods!)

    // Constructor
    Job(const Parameters& params, int commSize, int worldRank, int jobId);
    
    // Mark the job as being subject of a commitment to the given job request.
    // Requires the job to be not active and not committed.
    void commit(const JobRequest& req);
    // Unmark the job as being subject of a commitment to some job request.
    // Requires the job to be in a committed state.
    void uncommit();
    
    // Internally initializes the job (if necessary) and begins solving.
    void start(std::shared_ptr<std::vector<uint8_t>> data);
    // Interrupt the execution of all internal solvers.
    void stop();
    
    // Freeze the execution of all internal solvers. They can be resumed at any time.
    void suspend();
    // Resume all internal solvers given that they were frozen.
    void resume();
    
    bool wantsToCommunicate();
    // Initiate a communication with other nodes in the associated job tree.
    void communicate();

    // Interrupt the execution of solvers and withdraw the associated solvers 
    // and the job's payload. Only leaves behind the job's meta data.
    void terminate();

    void updateJobTree(int index, int rootRank, int parentRank);


    // Getter methods and simple queries

    // ... for the job state

    // Simple getter for current job state.
    JobState getState() const {return _state;};
    void assertState(JobState state) const {assert(_state == state || Console::fail("State of %s : %s", toStr(), jobStateToStr()));};

    // ... for the job's current meta data

    int getVolume() const {return _volume;}
    float getPriority() const {return _priority;}
    bool hasDescription() const {return _description.has_value();};
    JobDescription& getDescription() {assert(hasDescription()); return _description.value();};
    const JobDescription& getDescription() const {assert(hasDescription()); return _description.value();};
    std::shared_ptr<std::vector<uint8_t>>& getSerializedDescription() {return _serialized_description;};
    bool hasCommitment() const {return _commitment.has_value();}
    const JobRequest& getCommitment() const {assert(hasCommitment()); return _commitment.value();}
    int getId() const {return _id;};
    int getIndex() const {return _job_tree.getIndex();};
    int getRevision() const {return getDescription().getRevision();};
    const JobResult& getResult();
    // Elapsed seconds since the job's constructor call.
    float getAge() const {return Timer::elapsedSeconds() - _time_of_arrival;}
    // Elapsed seconds since initialization was ended.
    float getAgeSinceActivation() const {return Timer::elapsedSeconds() - _time_of_initialization;}
    // Elapsed seconds since termination of the job.
    float getAgeSinceAbort() const {return Timer::elapsedSeconds() - _time_of_abort;}
    float getAgeSinceLastLimitCheck() const {return Timer::elapsedSeconds() - std::max(_time_of_arrival, _time_of_last_limit_check);}
    float getUsedCpuSeconds() const {return _used_cpu_seconds;}

    // Return true iff this job instance has found a job result that it still needs to communicate.
    bool isResultTransferPending() const {return _result_transfer_pending;}
    // Returns whether the job is easily and quickly destructible as of now. 
    // (calls appl_isDestructible())
    bool isDestructible();
    
    int getGlobalNumWorkers() const {return _job_tree.getCommSize();}
    int getMyMpiRank() const {return _job_tree.getRank();}
    JobTree& getJobTree() {return _job_tree;}
    const JobTree& getJobTree() const {return _job_tree;}

    // Setter methods and simple manipulations

    void unpackDescription(std::shared_ptr<std::vector<uint8_t>> data) {
        _description.emplace();
        _description.value().deserialize(*data);
        _priority = _description.value().getPriority();
    };

    bool testReadyToGrow() {
        auto lock = _job_manipulation_lock.getLock();
        bool allJoined = !_unpack_threads.empty();
        for (size_t i = 0; i < _unpack_threads.size(); i++) {
            if (_unpack_threads[i].joinable() && _unpack_done[i]) {
                // Thread
                _unpack_threads[i].join();
            } else {
                allJoined = false;
                break;
            }
        }
        if (allJoined) {
            _unpack_threads.clear();
            _unpack_done.clear();
            return true;
        }
        return false;
    }
    bool isReadyToGrow() {
        auto lock = _job_manipulation_lock.getLock();
        return _unpack_threads.empty() && hasDescription();
    }

    void updateVolumeAndUsedCpu(int newVolume) {
        // Compute used CPU time within last time slice
        float time = Timer::elapsedSeconds();
        _used_cpu_seconds += (time - _time_of_last_limit_check) * _threads_per_job * _volume;
        _time_of_last_limit_check = time;
        // Update volume
        _volume = newVolume;
    }

    bool checkResourceLimit(float wcSecsPerInstance, float cpuSecsPerInstance) {

        updateVolumeAndUsedCpu(getVolume());
        float usedWcSecs = getAge();
        float usedCpuSecs = getUsedCpuSeconds();

        if ((cpuSecsPerInstance > 0 && usedCpuSecs > cpuSecsPerInstance)
            || (isReadyToGrow() && getDescription().getCpuLimit() > 0 && 
                usedCpuSecs > getDescription().getCpuLimit())) {
            // Job exceeded its cpu time limit
            Console::log(Console::INFO, "#%i CPU TIMEOUT: aborting", _id);
            return true;
        }

        if ((wcSecsPerInstance > 0 && usedWcSecs > wcSecsPerInstance) 
                || (isReadyToGrow() && getDescription().getWallclockLimit() > 0 && 
                    usedWcSecs > getDescription().getWallclockLimit())) {
            // Job exceeded its wall clock time limit
            Console::log(Console::INFO, "#%i WALLCLOCK TIMEOUT: aborting", _id);
            return true;
        }

        return false;
    }

    // ... of various meta data

    void setResultTransferPending(bool pending) {_result_transfer_pending = pending;}


    // toString methods

    const char* toStr() const {
        return _name.c_str();
    };
    const char* jobStateToStr() const {return jobStateStrings[(int)_state];};
};

#endif