
#pragma once

#include "app/job.hpp"
#include "app/sat/proof/palrup_caller.hpp"
#include "util/sys/thread_pool.hpp"
#include "util/sys/threading.hpp"

class PalrupCheckJob : public Job {

private:
    // Struct for the final result of the task.
    JobResult _result;
    std::future<void> _fut_done;

public:
    // Standard constructor. During construction, no job description is present yet.
    PalrupCheckJob(const Parameters& params, const JobSetup& setup, AppMessageTable& table) 
        : Job(params, setup, table) {

        // no result present
        _result.id = getId();
        _result.revision = 0;
        _result.result = -1;
    }
    ~PalrupCheckJob() {}

    // Begin your local computation
    void appl_start() override {
        _fut_done = ProcessWideThreadPool::get().addTask([&]() {
            const std::string cnfPath = getDescription().getAppConfiguration().map.at("__chkcnf");
            const std::string proofDir = getDescription().getAppConfiguration().map.at("__chkproofdir");
            auto res = PalRupCaller(_params, getGlobalNumWorkers(), cnfPath, proofDir).callBlocking();
            if (res == PalRupCaller::VALIDATED) _result.result = 20;
            if (res == PalRupCaller::ERROR) _result.result = 10;
        });
    }

    // Called periodically by the main thread to allow the worker to emit messages.
    void appl_communicate() override {
    }

    // Called periodically to query if a result/solution has been found at this worker.
    int appl_solved() override {
        if (_result.result != -1) {
            _result.setSolutionToSerialize(&_result.result, 1);
        }
        return _result.result; // -1 if no solution yet
    }
    // Return the result object.
    JobResult&& appl_getResult() override {return std::move(_result);}

    // Return whether this worker can be cleaned up without any blocking or waiting.
    // In our case, this is the case when no background task is pending.
    // Note that, as long as this returns false, 
    bool appl_isDestructible() override {
        // you can, and need to, advance communication at this point
        // so that everything that is still going on can conclude nicely
        if (_fut_done.valid() && Future::isPending(_fut_done)) return false;
        return true; // all communication and background computation concluded
    }

    // We don't need to implement the following few methods.

    // Suspend your local computation.
    void appl_suspend() override {}
    // Resume your local computation.
    void appl_resume() override {}
    // Terminate this worker unrecoverably.
    // We acknowledge this implicitly by having background threads check for getState().
    void appl_terminate() override {if (_fut_done.valid()) _fut_done.get();}
    // React to an incoming message. (This becomes relevant only if you send custom messages)
    void appl_communicate(int source, int mpiTag, JobMessage& msg) override {}
    // Print out application-specific statistics (called periodically)
    void appl_dumpStats() override {}
    // Reduce your main memory requirements as far as possible
    void appl_memoryPanic() override {}
};
