
#pragma once

#include "app/job.hpp"
#include "comm/msgtags.h"
#include "data/job_transfer.hpp"
#include "util/logger.hpp"
#include "util/permutation.hpp"

// This example job implementation performs a point-to-point round trip
// of a message across exactly eight workers.
// The message's route follows a pseudo-random permutation.
class PointToPointExampleJob : public Job {

private:
    static const int MSG_ROUNDTRIP = 1; // internal message tag for our round-trip messages
    static const int NUM_WORKERS = 8; // # workers we request and require

    // Represents a pseudo-random permutation of a set of integers [0..n).
    AdjustablePermutation _perm;
    // Struct for the final result of the task.
    JobResult _result;

    // Whether we already started our roundtrip.
    bool _started_roundtrip {false};

public:
    // Standard constructor. During construction, no job description is present yet.
    PointToPointExampleJob(const Parameters& params, const JobSetup& setup, AppMessageTable& table) 
        : Job(params, setup, table) {

        // Sending job messages along the job tree (i.e., to a direct parent or child)
        // is very easy via the convenience methods like getJobTree().sendToParent(msg) etc.
        // In our case, since we want to send messages to arbitrary workers in our tree,
        // we need a JobComm instance to be constructed for us in the background.
        assert(_params.jobCommUpdatePeriod() > 0 || log_return_false("[ERROR] For this application to work,"
            " you must explicitly enable job communicators with the -jcup option, e.g., -jcup=0.1\n"));
        // no result present
        _result.result = -1;
    }
    ~PointToPointExampleJob() {} // nothing to do

    int getDemand() const override {
        // return Job::getDemand();
        return NUM_WORKERS; // we strictly want this number of workers
    }

    // Begin your local computation
    void appl_start() override {
        // Initialize pseudo-random permutation with the number of workers -
        // use the 1st integer in the job's payload as a random seed
        _perm = AdjustablePermutation(NUM_WORKERS, getDescription().getFormulaPayload(0)[0]);
    }

    // Called periodically by the main thread to allow the worker to emit messages.
    void appl_communicate() override {

        // Not enough workers available?
        if (getJobTree().isRoot() && !_started_roundtrip && getVolume() < NUM_WORKERS) {
            if (getAgeSinceActivation() < 1) return; // wait for up to 1s after appl_start

            LOG(V2_INFO, "[dummy] Unable to get %i workers within 1 second - giving up\n", NUM_WORKERS);
            // Report an "unknown" result (code 0)
            insertResult(0, {-1});
            _started_roundtrip = true;
            return;
        }

        // Workers available and valid job communicator present?
        if (getJobTree().isRoot() && !_started_roundtrip && getVolume() == NUM_WORKERS
                && getJobComm().getWorldRankOrMinusOne(NUM_WORKERS-1) >= 0) {

            // craft a message to ping-pong around
            _started_roundtrip = true;
            JobMessage msg = getMessageTemplate();
            msg.tag = MSG_ROUNDTRIP;
            msg.payload = {0}; // indicates the number of bounces so far
            LOG(V2_INFO, "[dummy] starting round trip\n");
            advancePingPongMessage(msg);
        }
    }

    // React to an incoming message. (This becomes relevant only if you send custom messages)
    void appl_communicate(int source, int mpiTag, JobMessage& msg) override {
        int depth = msg.payload[0];
        LOG(V2_INFO, "[dummy] received round trip msg: %i bounces\n", depth);
        if (depth == NUM_WORKERS) {
            LOG(V2_INFO, "[dummy] round trip finished!\n");
            insertResult(10, std::vector<int>(msg.payload.begin()+1, msg.payload.end()));
        } else {
            advancePingPongMessage(msg);
        }
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
        return true; // all stateful communication and background computation concluded
    }


    // We don't need to implement the following few methods in our simple example.

    // Suspend your local computation.
    void appl_suspend() override {}
    // Resume your local computation.
    void appl_resume() override {}
    // Terminate this worker unrecoverably.
    void appl_terminate() override {}
    // Print out application-specific statistics (called periodically)
    void appl_dumpStats() override {}
    // Reduce your main memory requirements as far as possible
    void appl_memoryPanic() override {}

private:
    // Process an incoming (or internally crafted) round-trip message and,
    // if possible, advance it by one more bounce.
    void advancePingPongMessage(JobMessage& msg) {

        // msg.payload[0] is the number of bounces the message already did
        int permutedIndex = _perm.get(msg.payload[0]);
        // Use our JobComm to convert the tree index into an addressable MPI rank.
        int recvRank = getJobComm().getWorldRankOrMinusOne(permutedIndex);
        LOG(V2_INFO, "[dummy] next job tree index of round trip: %i, rank: %i\n", permutedIndex, recvRank);

        if (recvRank == -1) {
            // The job communicator has no valid rank for this job tree index!
            // Reset the round-trip by sending a fresh message to the root.
            msg.payload = {0};
            getJobTree().sendToRoot(msg);
        } else {
            // Found a valid rank!
            // Add this process to the history, but not if it's the very first one (the root)
            if (msg.payload[0] > 0) msg.payload.push_back(getJobTree().getRank());
            msg.payload[0]++; // add one bounce
            // We need to add these addressing values to the message explicitly because we're
            // messaging an arbitrary worker in the tree. For direct relatives, it is easier
            // to use the according convenience methods like getJobTree().sendToParent(msg).
            msg.treeIndexOfDestination = permutedIndex;
            msg.contextIdOfDestination = getJobComm().getContextIdOrZero(permutedIndex);
            assert(msg.contextIdOfDestination != 0);
            // Send
            getJobTree().send(recvRank, MSG_SEND_APPLICATION_MESSAGE, msg);
        }
    }

    // Mark the job as done, with the provided result code and solution.
    void insertResult(int resultCode, const std::vector<int>& solution) {
        _result.id = getId();
        _result.revision = getRevision();
        _result.result = resultCode;
        _result.setSolutionToSerialize(solution.data(), solution.size());
    }
};
