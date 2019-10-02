
#include <map>

#include "assert.h"
#include "util/console.h"
#include "data/job_image.h"

JobImage::JobImage(int commSize, int worldRank, int jobId) :
            commSize(commSize), worldRank(worldRank), jobId(jobId),
            hasDescription(false), initialized(false),
            jobNodeRanks(commSize, jobId) {}

void JobImage::store(Job job) {
    this->job = job;
    if (state == JobState::NONE) {
        state = JobState::STORED;
    }
    hasDescription = true;
}

void JobImage::initialize(int index, int rootRank, int parentRank) {

    this->index = index;
    updateJobNode(0, rootRank);
    updateJobNode(index/2, parentRank);
    updateJobNode(index, worldRank);

    initialize();
}

void JobImage::initialize() {

    assert(hasDescription);
    assert(state != JobState::ACTIVE && state != JobState::PAST);

    job.setVolume(1);
    job.setTemperature(100);

    std::map<std::string, std::string> params;
    params["e"] = "1"; // exchange mode: 0 = nothing, 1 = alltoall, 2 = log, 3 = asyncrumor
    params["c"] = "2"; // solvers count on this node
    params["d"] = "7"; // sparse random + native diversification
    params["mpirank"] = std::to_string(index); // mpi_rank
    params["mpisize"] = std::to_string(commSize); // mpi_size
    params["jobstr"] = toStr();

    solver = std::unique_ptr<HordeLib>(new HordeLib(params));
    solver->setFormula(job.getFormula());

    state = JobState::ACTIVE;
    solver->beginSolving();
    initialized = true;
}

void JobImage::reinitialize(int index, int rootRank, int parentRank) {

    if (!initialized) {

        initialize(index, rootRank, parentRank);

    } else {

        if (index == this->index) {

            // Resume the exact same work that you already did
            updateJobNode(index/2, parentRank);
            assert(solver != NULL);

            Console::log("Resuming Hordesat solving threads of " + toStr());
            resume();

        } else {
            this->index = index;

            // Restart clean permutation
            jobNodeRanks.clear();
            updateJobNode(0, rootRank);
            updateJobNode(index/2, parentRank);
            updateJobNode(index, worldRank);

            Console::log("Restarting Hordesat instance of " + toStr());
            solver->beginSolving();

            state = JobState::ACTIVE;
        }
    }
}

void JobImage::commit(const JobRequest& req) {

    assert(state != JobState::ACTIVE && state != JobState::COMMITTED);

    state = JobState::COMMITTED;
    index = req.requestedNodeIndex;
    updateJobNode(0, req.rootRank);
    updateJobNode((index-1)/2, req.requestingNodeRank);
    updateJobNode(index, worldRank);
}

void JobImage::uncommit(const JobRequest& req) {

    assert(state == JobState::COMMITTED);

    if (initialized) {
        state = JobState::SUSPENDED;
    } else if (hasDescription) {
        state = JobState::STORED;
    } else {
        state = JobState::NONE;
    }
    jobNodeRanks.clear();
}

void JobImage::setLeftChild(int rank) {
    leftChild = true;
    updateJobNode(getLeftChildIndex(), rank);
}
void JobImage::setRightChild(int rank) {
    rightChild = true;
    updateJobNode(getRightChildIndex(), rank);
}

std::vector<int> JobImage::collectClausesFromSolvers() {
    return solver->prepareSharing( commSize /*job.getVolume()*/);
}
void JobImage::insertIntoClauseBuffer(std::vector<int>& vec) {

    clausesToShare.insert(clausesToShare.end(), vec.begin(), vec.end());

    int prevSize = clausesToShare.size();
    int remainder = prevSize % BROADCAST_CLAUSE_INTS_PER_NODE;
    if (remainder != 0) {
        clausesToShare.resize(prevSize + BROADCAST_CLAUSE_INTS_PER_NODE-remainder);
        for (unsigned int i = prevSize; i < clausesToShare.size(); i++) {
            clausesToShare[i] = 0;
        }
    }

    assert(clausesToShare.size() % BROADCAST_CLAUSE_INTS_PER_NODE == 0);
}
void JobImage::collectClausesFromBelow(std::vector<int>& clauses) {

    insertIntoClauseBuffer(clauses);
    sharedClauseSources++;
}
bool JobImage::canShareCollectedClauses() {
    int numChildren = 0;
    // Must have received clauses from both children,
    // except if one / both of them cannot exist according to volume
    if (hasLeftChild()) numChildren++;
    if (hasRightChild()) numChildren++;
    return numChildren == sharedClauseSources;
}
std::vector<int> JobImage::shareCollectedClauses() {
    std::vector<int> selfClauses = collectClausesFromSolvers();
    insertIntoClauseBuffer(selfClauses);
    std::vector<int> vec = clausesToShare;
    sharedClauseSources = 0;
    clausesToShare.resize(0);
    return vec;
}
void JobImage::learnClausesFromAbove(std::vector<int>& clauses) {
    solver->digestSharing(clauses);
}

void JobImage::suspend() {
    assert(state == JobState::ACTIVE);
    state = JobState::SUSPENDED;
    solver->setPaused();
    Console::log("Suspended Hordesat solving threads of " + toStr());
}

void JobImage::resume() {
    assert(solver->isRunning());
    solver->unsetPaused();
    Console::log("Resumed Hordesat solving threads of " + toStr());
    state = JobState::ACTIVE;
}

void JobImage::withdraw() {

    assert(state == JobState::ACTIVE || state == JobState::SUSPENDED);

    solver->setTerminate(); // sets the "solvingDoneLocal" flag in the solver
    solver->unsetPaused(); // if solver threads are suspended, wake them up to recognize termination
    solver->finishSolving(); // joins threads and concludes solving process

    state = JobState::PAST;
    leftChild = false;
    rightChild = false;
    doneLocally = false;
}

int JobImage::solveLoop() {

    int result = -1;

    if (doneLocally) {
        // Already reported the actual result; solver sleeps
        return result;
    }

    if (state == JobState::ACTIVE) {
        // if result is found, stops all solvers
        // but does not call finishSolving()
        result = solver->solveLoop();
    }
    if (result >= 0) {
        doneLocally = true;
    }
    return result;
}
