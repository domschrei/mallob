
#pragma once

#include "app/job.hpp"
#include "comm/job_tree_broadcast.hpp"
#include "comm/job_tree_all_reduction_modular.hpp"
#include "data/job_state.h"
#include "data/job_transfer.hpp"
#include "util/logger.hpp"
#include "util/sys/thread_pool.hpp"
#include "util/sys/threading.hpp"
#include <future>

// This job does some internal computation at the root worker,
// then broadcasts the result to every worker currently in the tree,
// then performs an all-reduction across these workers where each worker
// contributes a value based on the original broadcasted value,
// and finally has a leaf worker report the all-reduction's broadcasted result
// as the result to the job.
class CollectivesExampleJob : public Job {

private:
    // A JobTreeBroadcast instance represents one single, plain broadcast along the job tree.
    std::unique_ptr<JobTreeBroadcast> _bcast;
    // A JobTreeAllReduction instance represents one single all-reduction along the job tree.
    std::unique_ptr<JobTreeAllReduction> _red;

    // Internal message tags which must be different for each pair of potentially concurrent
    // or directly adjacent collective operations.
    static const int BCAST_INIT {1};
    static const int ALLRED {2};

    // Synchronization primitives for some concurrent task at the root worker.
    std::promise<int> _promise_root_computation_result;
    std::future<int> _fut_root_computation_result;

    // Struct for the final result of the task.
    JobResult _result;

public:
    // Standard constructor. During construction, no job description is present yet.
    CollectivesExampleJob(const Parameters& params, const JobSetup& setup, AppMessageTable& table) 
        : Job(params, setup, table) {

        // no result present
        _result.result = -1;
    }
    ~CollectivesExampleJob() {
        // Ensure to free all associated resources!
        // In our case, wait for any background task to finish.
        if (_fut_root_computation_result.valid()) _fut_root_computation_result.get();
    }

    // Begin your local computation
    void appl_start() override {

        // Every worker initializes its own broadcast object
        // Take care that the provided message ID (3rd param) is always consistent, or omit this param
        // if you can guarantee that only one single broadcast is going on in this job at any time.
        LOG(V2_INFO, "[dummy] initialize broadcast object\n");
        _bcast.reset(new JobTreeBroadcast(getId(), getJobTree().getSnapshot(),
            [this]() {digestBroadcast();}, BCAST_INIT));

        // The root worker also launches a side task whose outcome is a message to broadcast
        if (getJobTree().isRoot()) {

            // link future and promise objects
            _fut_root_computation_result = _promise_root_computation_result.get_future();

            // launch background task via thread pool
            ProcessWideThreadPool::get().addTask([this]() {
                // Perform some heavy computation
                int res = 0;
                for (int i = 1; i <= 10'000'000; i++) {
                    if (getState() == PAST) {
                        // terminated
                        res = -1;
                        break;
                    }
                    while (getState() == SUSPENDED) {
                        // interrupted
                        usleep(1000 * 10); // 10 milliseconds
                    }
                    res += i;
                }
                // Deposit the result in the promise/future
                _promise_root_computation_result.set_value(res);
            });
        }
    }

    // Called periodically by the main thread to allow the worker to emit messages.
    void appl_communicate() override {

        // Root: Update job tree snapshot in case your children changed
        if (_bcast && Future::isPending(_fut_root_computation_result))
            _bcast->updateJobTree(getJobTree());

        // See below for individual communication methods.
        tryBeginBroadcast();
        tryEndReduction();
    }

    // Called periodically to query if a result/solution has been found at this worker.
    int appl_solved() override {
        return _result.result; // -1 if no solution yet
    }
    // Return the result object.
    JobResult&& appl_getResult() override {return std::move(_result);}

    // Return whether this worker can be cleaned up without any blocking or waiting.
    // In our case, this is the case when no background task is pending.
    // Note that, as long as this returns false, 
    bool appl_isDestructible() override {
        if (_bcast) return false;
        // you can, and need to, advance communication at this point
        // so that everything that is still going on can conclude nicely
        if (_red) appl_communicate();
        if (_red) return false;
        return true; // all communication and background computation concluded
    }


    // We don't need to implement the following few methods in our simple example.

    // Suspend your local computation.
    void appl_suspend() override {}
    // Resume your local computation.
    void appl_resume() override {}
    // Terminate this worker unrecoverably.
    // We acknowledge this implicitly by having background threads check for getState().
    void appl_terminate() override {}
    // React to an incoming message. (This becomes relevant only if you send custom messages)
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
        msg.tag = _bcast->getMessageTag();
        msg.payload = {_fut_root_computation_result.get()};
        _bcast->broadcast(std::move(msg));
    }

    void digestBroadcast() {
        // Valid broadcast object, a broadcast result is present
        assert(_bcast);
        assert(_bcast->hasResult());

        // Retrieve result of broadcast
        auto res = _bcast->getJobMessage();
        auto snapshot = _bcast->getJobTreeSnapshot();
        LOG(V2_INFO, "[dummy] received broadcast \"%i\", %i children in snapshot\n", res.payload.front(), snapshot.nbChildren);
        _bcast.reset();

        JobMessage baseMsg = getMessageTemplate();
        baseMsg.tag = ALLRED;
        _red.reset(new JobTreeAllReduction(snapshot, baseMsg, std::vector<int>(), [](std::list<std::vector<int>>& contribs) {
            int sum = 0;
            for (auto& contrib : contribs) sum += contrib.at(0);
            return std::vector<int>(1, sum);
        }));
        const int contrib = res.payload.front() * getJobTree().getRank();
        LOG(V2_INFO, "[dummy] contribute %i to all-reduction\n", contrib);
        _red->contribute({contrib});
    }

    void tryEndReduction() {
        if (!_red) return;
        if (!_red->advance().hasResult()) return;

        LOG(V2_INFO, "[dummy] all-reduction complete\n");
        auto result = _red->extractResult();

        // Is this worker a leaf in the broadcast that was just performed?
        if (_red->getJobTreeSnapshot().nbChildren == 0) {
            // Store the result of the broadcast as a result to this task
            // (This will be noticed by appl_solved and results in the task's termination)
            _result.id = getId();
            _result.revision = getRevision();
            _result.result = 10;
            _result.setSolutionToSerialize(result.data(), result.size());
        }

        // Conclude the all-reduction, allowing for this worker to be destructed later
        _red.reset();
    }
};
