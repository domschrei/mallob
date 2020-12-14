
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
                             v
    PAST <--terminate()-- INACTIVE --start()--> ACTIVE --suspend()--> STANDBY
                                   <--stop()---        <--resume()---

    General notes:
    * Communication (MyMpi::*) may ONLY take place within mallob's main thread, i.e., 
      within the appl_* methods (and not a thread spawned from it).
    * All appl_* methods must return immediately. Heavy work must be performed concurrently
      within a separate thread. Exceptions are appl_beginCommunication and appl_communicate
      where a decent amount of work of the main thread may be necessary to handle communication.
    * Your code must bookkeep the active threads of your application, make them ready to be joined 
      after appl_terminate() has been called, and be able to instantly join them if
      your appl_isDestructible() returns true. 
    * If your application spawns child processes, appl_suspend() and appl_resume() can be
      realized over SIGTSTP and SIGCONT signals, but also make sure that the process is
      terminated after appl_terminate().

    */

    /*
    Begin, or continue, to process the job.
    At the first call of appl_start(), you can safely assume that the job description
    is already present: getDescription() returns a valid JobDescription instance.
    appl_start() may be called again after appl_stop(); in this case, the internal job data
    (i.e. its description) may have changed and must be reconsidered.
    */
    virtual void appl_start() = 0;
    /*
    Stop to process an active job.
    */
    virtual void appl_stop() = 0;
    /*
    Suspend the job, enabling a resumption of the job processing at a later time.
    After suspension is completed, the job should not spend any CPU time on itself any more
    (with the possible exception of concurrent initializer threads that are still running).
    */
    virtual void appl_suspend() = 0;
    /*
    Resume a suspended job in the same internal state where it was suspended.
    */
    virtual void appl_resume() = 0;
    /*
    Terminate an inactive job which is irrecoverably marked for deletion: This method
    can concurrently trigger a cleanup of any resources needed for solving the job.
    */
    virtual void appl_terminate() = 0;
    /*
    Check from the main thread whether any solution was already found.
    Return a result code if applicable, or -1 otherwise.
    This method can be used to perform further (quick) checks regarding the application
    and can be expected to be called frequently while the job is active.
    */
    virtual int appl_solved() = 0;
    /*
    appl_solved() returned a result >= 0; return a fitting JobResult instance at this point.
    This method must only be valid once after a solution has been found.
    */
    virtual JobResult appl_getResult() = 0;
    /*
    Signal if this job instance would like to initiate a new job communication phase.
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
    This method must return an integer greater than 0 and no greater than _comm_size. 
    It has a valid default implementation, so it does not need to be re-implemented.
    */
    virtual int getDemand(int prevVolume, float elapsedTime = Timer::elapsedSeconds()) const;

    /*
    Return true iff the instance can be quickly deleted without any errors
    (i.e., no running concurrent thread of unknown residual life time depends 
    on the instance any more, and the destructor will return immediately).
    */
    virtual bool appl_isDestructible() = 0;
    /*
    Free all data associated to this job instance. Join all associated threads if any are left.
    */
    virtual ~Job() {
        for (auto& thread : _unpack_threads) if (thread.joinable()) thread.join();
    }
    
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
    bool _has_description = false;
    JobDescription _description;
    std::shared_ptr<std::vector<uint8_t>> _serialized_description;

    std::vector<std::thread> _unpack_threads;
    std::vector<bool> _unpack_done;

    float _time_of_arrival;
    float _time_of_activation = 0;
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
    
    // Concurrently unpacks the job description and then calls the job implementation's
    // appl_start() method from there.
    void start(std::shared_ptr<std::vector<uint8_t>> data);
    // Interrupt the execution of all internal solvers.
    void stop();
    
    // Freeze the execution of all internal solvers. They can be resumed at any time.
    void suspend();
    // Resume all internal solvers given that they were frozen.
    void resume();
    
    // Returns true if communicate() should be called.
    bool wantsToCommunicate();
    // Initiate a communication with other nodes in the associated job tree.
    void communicate();

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
    void assertState(JobState state) const {assert(_state == state || Console::fail("State of %s : %s", toStr(), jobStateToStr()));};
    int getVolume() const {return _volume;}
    float getPriority() const {return _priority;}
    bool hasReceivedDescription() const {return _serialized_description != nullptr;};
    bool hasDeserializedDescription() const {return _has_description;};
    const JobDescription& getDescription() const {assert(hasDeserializedDescription()); return _description;};
    std::shared_ptr<std::vector<uint8_t>>& getSerializedDescription() {return _serialized_description;};
    bool hasCommitment() const {return _commitment.has_value();}
    const JobRequest& getCommitment() const {assert(hasCommitment()); return _commitment.value();}
    int getId() const {return _id;};
    int getIndex() const {return _job_tree.getIndex();};
    int getRevision() const {assert(hasDeserializedDescription()); return getDescription().getRevision();};
    const JobResult& getResult();
    // Elapsed seconds since the job's constructor call.
    float getAge() const {return Timer::elapsedSeconds() - _time_of_arrival;}
    // Elapsed seconds since initialization was ended.
    float getAgeSinceActivation() const {return Timer::elapsedSeconds() - _time_of_activation;}
    // Elapsed seconds since termination of the job.
    float getAgeSinceAbort() const {return Timer::elapsedSeconds() - _time_of_abort;}
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


    // Tests if all unpacking of job descriptions is finished
    // and, consequently, the job is ready to forward its description
    // to children itself. The method returns true only once;
    // repeated calls about the growth readiness can be made via isReadyToGrow().
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
        return _unpack_threads.empty() && hasDeserializedDescription();
    }

    // Updates the job's resource usage based on the period of time which passed
    // since the last call (or the job's activation) and the old volume of the job,
    // and then updates the volume itself.
    void updateVolumeAndUsedCpu(int newVolume) {
        // Compute used CPU time within last time slice
        float time = Timer::elapsedSeconds();
        _used_cpu_seconds += (time - _time_of_last_limit_check) * _threads_per_job * _volume;
        _time_of_last_limit_check = time;
        // Update volume
        _volume = newVolume;
    }

    // Updates the job's resource usage and then checks whether the job reached
    // a limit imposed by its description or by a global parameter.
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

    // Marks the job to be indestructible as long as pending is true.
    void setResultTransferPending(bool pending) {_result_transfer_pending = pending;}


    // toString methods

    const char* toStr() const {
        return _name.c_str();
    };
    const char* jobStateToStr() const {return jobStateStrings[(int)_state];};


private:

    void unpackDescription(std::shared_ptr<std::vector<uint8_t>> data) {
        _description.deserialize(*data);
        _priority = _description.getPriority();
        _has_description = true;
    };
};

#endif