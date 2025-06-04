
#ifndef DOMPASCH_MALLOB_DUMMY_JOB_HPP
#define DOMPASCH_MALLOB_DUMMY_JOB_HPP

#include "app/job.hpp"
#include "comm/job_tree_broadcast.hpp"
#include "data/job_transfer.hpp"
#include "util/logger.hpp"
#include "util/sys/thread_pool.hpp"
#include "util/sys/threading.hpp"
#include <future>

/*
Small dummy example "application" for a Mallob job. 
Edit and extend for your application. 
*/
class DummyJob : public Job {

private:
    // A JobTreeBroadcast instance represents one single, plain broadcast along the job tree.
    // reset() for every broadcast.
    std::unique_ptr<JobTreeBroadcast> _bcast;
    // Unique tag for each broadcast.
    int _msg_tag_bcast {1};

    // Synchronization primitives for some concurrent task at the root worker.
    std::promise<int> _promise_root_computation_result;
    std::future<int> _fut_root_computation_result;

    // Struct for the final result of the task.
    JobResult _result;

public:
    // Standard constructor. During construction, no job description is present yet.
    DummyJob(const Parameters& params, const JobSetup& setup, AppMessageTable& table) 
        : Job(params, setup, table) {

        // no result present
        _result.result = -1;
    }
    ~DummyJob() {
        // Ensure to free all associated resources!
        // In our case, wait for any background task to finish.
        if (_fut_root_computation_result.valid()) _fut_root_computation_result.get();
    }

    // Begin your local computation
    void appl_start() override {

        // Every worker initializes its own broadcast object
        // Take care that the provided message ID is always consistent, or omit this parameter
        // if you can guarantee that only one single broadcast is going on in this job at any time.
        LOG(V2_INFO, "[dummy] initialize broadcast object\n");
        _bcast.reset(new JobTreeBroadcast(getId(), getJobTree(), _msg_tag_bcast++));

        // The root worker also launches a side task whose outcome is a message to broadcast
        if (getJobTree().isRoot()) {

            // link future and promise objects
            _fut_root_computation_result = _promise_root_computation_result.get_future();

            // launch background task via thread pool
            ProcessWideThreadPool::get().addTask([&]() {
                // Perform some heavy computation
                int res = 0;
                for (int i = 1; i <= 30'000'000; i++) {
                    res += i*i;
                }
                // Deposit the result in the promise/future
                _promise_root_computation_result.set_value(res);
            });
        }
    }

    // Called periodically by the main thread to allow the worker to emit messages.
    void appl_communicate() override {
        // See below for the individual communication methods.
        tryBeginBroadcast();
        tryDigestBroadcast();
    }

    // Called periodically to query if a result/solution has been found at this worker.
    int appl_solved() override {
        return _result.result; // -1 if no solution yet
    }
    // Return the result object.
    JobResult&& appl_getResult() override {return std::move(_result);}

    // Terminate this worker unrecoverably.
    void appl_terminate() override {
        // Deleting our broadcast object from the main thread is generally fine -
        // later messages coming in will then be returned to their sender
        _bcast.reset();
    }

    // Return whether this worker can be cleaned up without any blocking or waiting.
    // In our case, this is the case when no background task is pending.
    bool appl_isDestructible() override {return Future::isPending(_fut_root_computation_result);}


    // We don't need to implement the following few methods for our simple example.

    // Suspend your local computation.
    void appl_suspend() override {}
    // Resume your local computation.
    void appl_resume() override {}
    // React to an incoming message. (This becomes relevant if you send custom messages)
    void appl_communicate(int source, int mpiTag, JobMessage& msg) override {}
    // Print out application-specific statistics (called periodically)
    void appl_dumpStats() override {}
    // Reduce your main memory requirements as far as possible
    void appl_memoryPanic() override {}

private:

    void tryBeginBroadcast() {
        // Conditions: Valid broadcast object, future for background task is valid and ready
        if (!_bcast) return;
        if (!Future::isValidAndReady(_fut_root_computation_result)) return;

        // Broadcast a message to all workers in your (sub) tree
        JobMessage msg = getMessageTemplate();
        msg.payload = {_fut_root_computation_result.get()};
        msg.tag = _bcast->getMessageTag();
        _bcast->broadcast(std::move(msg));
    }

    void tryDigestBroadcast() {
        // Conditions: Valid broadcast object, a broadcast result is present
        if (!_bcast) return;
        if (!_bcast->hasResult()) return;

        // Retrieve result of broadcast
        auto res = _bcast->getJobMessage();
        auto snapshot = _bcast->getJobTreeSnapshot();
        LOG(V2_INFO, "[dummy] received broadcast \"%i\"\n", res.payload.front());

        // Is this worker a leaf in the broadcast that was just performed?
        if (snapshot.nbChildren == 0) {
            // Store the result of the broadcast as a result to this task
            // (This will be noticed by appl_solved and results in the task's termination)
            _result.id = getId();
            _result.revision = getRevision();
            _result.result = 10;
            _result.setSolutionToSerialize(res.payload.data(), res.payload.size());
        }
    }
};

#endif
