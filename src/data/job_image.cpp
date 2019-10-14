
#include <map>
#include <thread>

#include "assert.h"
#include "util/console.h"
#include "util/timer.h"
#include "data/job_image.h"

JobImage::JobImage(Parameters& params, int commSize, int worldRank, int jobId, EpochCounter& epochCounter) :
            params(params), commSize(commSize), worldRank(worldRank), 
            jobId(jobId), epochCounter(epochCounter), epochOfArrival(epochCounter.get()), 
            elapsedSecondsOfArrival(Timer::elapsedSeconds()), 
            hasDescription(false), initialized(false), jobNodeRanks(commSize, jobId) {}

void JobImage::store(JobDescription& job) {
    this->job = job;
    if (state == NONE) {
        state = STORED;
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

    std::map<std::string, std::string> params;
    params["e"] = "1"; // exchange mode: 0 = nothing, 1 = alltoall, 2 = log, 3 = asyncrumor
    params["c"] = this->params.getParam("t"); // solver threads on this node
    params["d"] = "7"; // sparse random + native diversification
    params["i"] = "0"; // #microseconds to sleep during solve loop
    params["v"] = (this->params.getIntParam("v") >= 3 ? "1" : "0"); // verbosity
    params["mpirank"] = std::to_string(index); // mpi_rank
    params["mpisize"] = std::to_string(commSize); // mpi_size
    params["jobstr"] = toStr();
    solver = std::unique_ptr<HordeLib>(new HordeLib(params));
    doSolverInitialization();
}

void JobImage::doSolverInitialization() {

    assert(solver != NULL);

    solver->beginSolving(job.getFormula());
    initialized = true;
    state = ACTIVE;
}

void JobImage::reinitialize(int index, int rootRank, int parentRank) {

    if (!initialized) {

        initialize(index, rootRank, parentRank);

    } else {

        if (index == this->index) {

            // Resume the exact same work that you already did
            updateJobNode(index/2, parentRank);
            assert(solver != NULL);

            Console::log(Console::INFO, "Resuming Hordesat solving threads of " + toStr());
            resume();

        } else {
            this->index = index;

            // Restart clean permutation
            jobNodeRanks.clear();
            updateJobNode(0, rootRank);
            updateJobNode(index/2, parentRank);
            updateJobNode(index, worldRank);

            Console::log(Console::INFO, "Restarting Hordesat instance of " + toStr());
            solver->beginSolving();

            state = ACTIVE;
        }
    }
}

void JobImage::commit(const JobRequest& req) {

    assert(state != ACTIVE && state != COMMITTED);

    state = COMMITTED;
    index = req.requestedNodeIndex;
    updateJobNode(0, req.rootRank);
    updateJobNode((index-1)/2, req.requestingNodeRank);
    updateJobNode(index, worldRank);
}

void JobImage::uncommit(const JobRequest& req) {

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

void JobImage::setLeftChild(int rank) {
    leftChild = true;
    updateJobNode(getLeftChildIndex(), rank);
}
void JobImage::setRightChild(int rank) {
    rightChild = true;
    updateJobNode(getRightChildIndex(), rank);
}

std::vector<int> JobImage::collectClausesFromSolvers() {
    if (!solver->isFullyInitialized()) {
        return std::vector<int>(BROADCAST_CLAUSE_INTS_PER_NODE, 0);
    }
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
    if (!solver->isFullyInitialized())
        return; // discard clauses TODO keep?
    Console::log(Console::VERB, "Digesting clauses ...");
    solver->digestSharing(clauses);
    Console::log(Console::VERB, "Digested clauses.");
}

void JobImage::suspend() {
    assert(state == ACTIVE);
    solver->setPaused();
    state = SUSPENDED;
    Console::log(Console::INFO, "Suspended Hordesat solving threads of " + toStr());
}

void JobImage::resume() {
    
    if (!initialized) {
        initialize(index, getRootNodeRank(), getParentNodeRank());
    } else {
        solver->unsetPaused();
        Console::log(Console::INFO, "Resumed Hordesat solving threads of " + toStr());
        state = ACTIVE;
    }
}

void JobImage::withdraw() {

    assert(state == ACTIVE || state == SUSPENDED);

    solver->setTerminate(); // sets the "solvingDoneLocal" flag in the solver
    solver->unsetPaused(); // if solver threads are suspended, wake them up to recognize termination
    solver->finishSolving(); // joins threads and concludes solving process

    state = PAST;
    leftChild = false;
    rightChild = false;
    doneLocally = false;
}

int JobImage::solveLoop() {

    int result = -1;

    // Already reported the actual result, or still initializing
    if (doneLocally) {
        return result;
    }

    if (state == ACTIVE) {
        // if result is found, stops all solvers
        // but does not call finishSolving()
        result = solver->solveLoop();
    }
    if (result >= 0) {
        doneLocally = true;
    }
    return result;
}

int JobImage::getDemand() const {   
    return std::min(commSize, (int) std::pow(2, epochCounter.get() - epochOfArrival + 1) - 1);
}