
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

void Job::store(std::vector<int>& data) {
    setDescription(data);
    if (state == NONE) {
        this->index = -1;
        switchState(STORED);
    }
}

void Job::setDescription(std::vector<int>& data) {
    this->serializedDescription = data;
    this->job = JobDescription();
    this->job.deserialize(data);
    hasDescription = true;
}

void Job::initialize() {
    endInitialization();
}

void Job::beginInitialization() {
    switchState(INITIALIZING_TO_ACTIVE);
}

void Job::endInitialization() {
    initialized = true;
    switchState(ACTIVE);
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
        
        beginInitialization();
        initialize(index, rootRank, parentRank);

    } else {

        if (index == this->index) {

            // Resume the exact same work that you already did
            updateParentNodeRank(parentRank);

            Console::log(Console::INFO, "Resuming solvers of %s", toStr());
            resume();

        } else {
            this->index = index;

            // Restart clean permutation
            jobNodeRanks.clear();
            updateJobNode(0, rootRank);
            updateParentNodeRank(parentRank);
            updateJobNode(index, worldRank);

            Console::log(Console::INFO, "Restarting solvers of %s", toStr());
            beginSolving();

            switchState(ACTIVE);
        }
    }
}

void Job::commit(const JobRequest& req) {

    assert(state != ACTIVE && state != COMMITTED);

    index = req.requestedNodeIndex;
    updateJobNode(index, worldRank);
    if (index > 0) {
        updateJobNode(0, req.rootRank);
    }
    updateParentNodeRank(req.requestingNodeRank);
    switchState(COMMITTED);
}

void Job::uncommit(const JobRequest& req) {

    assert(state == COMMITTED);

    if (initialized) {
        switchState(SUSPENDED);
    } else if (hasDescription) {
        switchState(STORED);
    } else {
        switchState(NONE);
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
    if (isInState({INITIALIZING_TO_ACTIVE, INITIALIZING_TO_SUSPENDED})) {
        switchState(INITIALIZING_TO_SUSPENDED);
        return;
    }
    assert(state == ACTIVE);
    pause();
    switchState(SUSPENDED);
    Console::log(Console::INFO, "Suspended solving threads of %s", toStr());
}

void Job::resume() {
    if (isInState({INITIALIZING_TO_ACTIVE, INITIALIZING_TO_SUSPENDED})) {
        switchState(INITIALIZING_TO_ACTIVE);
        return;
    }
    if (!initialized) {
        initialize(index, getRootNodeRank(), getParentNodeRank());
    } else {
        unpause();
        switchState(ACTIVE);
        Console::log(Console::INFO, "Resumed solving threads of %s", toStr());
    }
}

void Job::withdraw() {

    if (isInState({INITIALIZING_TO_ACTIVE, INITIALIZING_TO_SUSPENDED})) {
        switchState(INITIALIZING_TO_PAST);
        return;
    } else {
        assert(state == ACTIVE || state == SUSPENDED);
    }

    terminate();

    leftChild = false;
    rightChild = false;
    doneLocally = false;
    switchState(PAST);
}

int Job::getDemand() const {   
    return std::min(commSize, (int) std::pow(2, epochCounter.getEpoch() - epochOfArrival + 1) - 1);
}

const JobResult& Job::getResult() const {
    assert(result.id >= 0); 
    return result;
}

bool Job::wantsToCommunicate() const {
    return isInState({ACTIVE}) && !hasLeftChild() && !hasRightChild() 
            && epochOfLastCommunication < (int)epochCounter.getEpoch() 
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

void Job::switchState(JobState state) {
    this->state = state;
    Console::log(Console::VERB, "%s : state \"%s\"", toStr(), jobStateStrings[state]); 
}