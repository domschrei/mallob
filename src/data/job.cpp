
#include <map>
#include <thread>

#include "assert.h"
#include "data/job.h"
#include "util/console.h"
#include "util/timer.h"

Job::Job(Parameters& params, int commSize, int worldRank, int jobId, EpochCounter& epochCounter) :
            params(params), commSize(commSize), worldRank(worldRank), 
            jobId(jobId), epochCounter(epochCounter), epochOfArrival(epochCounter.getEpoch()), 
            elapsedSecondsOfArrival(Timer::elapsedSeconds()), 
            hasDescription(false), initialized(false), jobNodeRanks(commSize, jobId) {}

void Job::store(JobDescription& job) {
    this->job = job;
    if (state == NONE) {
        state = STORED;
    }
    hasDescription = true;
}

void Job::initialize() {
    initialized = true;
    state = ACTIVE;
}

void Job::initialize(int index, int rootRank, int parentRank) {
    this->index = index;
    updateJobNode(0, rootRank);
    updateParentNodeRank(parentRank);
    updateJobNode(index, worldRank);
    initialize();
}

void Job::reinitialize(int index, int rootRank, int parentRank) {

    if (!initialized) {

        initialize(index, rootRank, parentRank);

    } else {

        if (index == this->index) {

            // Resume the exact same work that you already did
            updateParentNodeRank(parentRank);

            Console::log(Console::INFO, "Resuming solvers of " + toStr());
            resume();

        } else {
            this->index = index;

            // Restart clean permutation
            jobNodeRanks.clear();
            updateJobNode(0, rootRank);
            updateParentNodeRank(parentRank);
            updateJobNode(index, worldRank);

            Console::log(Console::INFO, "Restarting solvers of " + toStr());
            beginSolving();

            state = ACTIVE;
        }
    }
}

void Job::commit(const JobRequest& req) {

    assert(state != ACTIVE && state != COMMITTED);

    state = COMMITTED;
    index = req.requestedNodeIndex;
    updateJobNode(0, req.rootRank);
    updateParentNodeRank(req.requestingNodeRank);
    updateJobNode(index, worldRank);
}

void Job::uncommit(const JobRequest& req) {

    assert(state == COMMITTED);

    if (initialized) {
        state = SUSPENDED;
    } else if (hasDescription) {
        state = STORED;
    } else {
        state = NONE;
    }
    jobNodeRanks.clear();
}

void Job::setLeftChild(int rank) {
    leftChild = true;
    updateJobNode(getLeftChildIndex(), rank);
}
void Job::setRightChild(int rank) {
    rightChild = true;
    updateJobNode(getRightChildIndex(), rank);
}

void Job::suspend() {
    assert(state == ACTIVE);
    state = SUSPENDED;
    pause();
    Console::log(Console::INFO, "Suspended solving threads of " + toStr());
}

void Job::resume() {
    
    if (!initialized) {
        initialize(index, getRootNodeRank(), getParentNodeRank());
    } else {
        unpause();
        Console::log(Console::INFO, "Resumed solving threads of " + toStr());
        state = ACTIVE;
    }
}

void Job::withdraw() {

    assert(state == ACTIVE || state == SUSPENDED);

    terminate();

    state = PAST;
    leftChild = false;
    rightChild = false;
    doneLocally = false;
}

int Job::getDemand() const {   
    return std::min(commSize, (int) std::pow(2, epochCounter.getEpoch() - epochOfArrival + 1) - 1);
}

bool Job::wantsToCommunicate() const {
    return isInState({ACTIVE}) && !hasLeftChild() && !hasRightChild() 
            && epochOfLastCommunication < epochCounter.getEpoch() 
            && epochCounter.getSecondsSinceLastSync() >= 2.5f;
}

void Job::communicate() {
    beginCommunication();
    epochOfLastCommunication = epochCounter.getEpoch();
}

bool Job::isInState(std::initializer_list<JobState> list) const {
    for (JobState state : list) {
        if (state == this->state)
            return true;
    }
    return false;
}
bool Job::isNotInState(std::initializer_list<JobState> list) const {
    for (JobState state : list) {
        if (state == this->state)
            return false;
    }
    return true;
}