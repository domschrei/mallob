
#include <map>
#include <thread>

#include "assert.h"
#include "util/console.h"
#include "util/timer.h"
#include "util/mpi.h"
#include "data/sat_job.h"
#include "util/console_horde_interface.h"

void SatJob::initialize() {

    assert(isInState({INITIALIZING_TO_ACTIVE, INITIALIZING_TO_SUSPENDED, INITIALIZING_TO_PAST}));
    assert(hasDescription);

    Console::log(Console::VERB, "preparing params");
    std::map<std::string, std::string> params;
    params["e"] = "1"; // exchange mode: 0 = nothing, 1 = alltoall, 2 = log, 3 = asyncrumor
    params["c"] = this->params.getParam("t"); // solver threads on this node
    params["d"] = "7"; // sparse random + native diversification
    params["i"] = "0"; // #microseconds to sleep during solve loop
    params["v"] = (this->params.getIntParam("v") >= 3 ? "1" : "0"); // verbosity
    params["mpirank"] = std::to_string(index); // mpi_rank
    params["mpisize"] = std::to_string(commSize); // mpi_size
    std::string identifier = std::string(toStr());
    params["jobstr"] = identifier;
    Console::log(Console::VERB, "creating horde instance");
    solver = std::unique_ptr<HordeLib>(new HordeLib(params, std::shared_ptr<LoggingInterface>(new ConsoleHordeInterface(identifier))));
    
    assert(solver != NULL);

    Console::log(Console::VERB, "beginning to solve");
    solver->beginSolving(job.getPayload());
    Console::log(Console::VERB, "finished initialization");
}

void SatJob::beginSolving() {
    solver->beginSolving();
}

void SatJob::pause() {
    solver->setPaused();
}

void SatJob::unpause() {
    solver->unsetPaused();
}

void SatJob::terminate() {
    solver->setTerminate(); // sets the "solvingDoneLocal" flag in the solver
    solver->unsetPaused(); // if solver threads are suspended, wake them up to recognize termination
    solver->finishSolving(); // joins threads and concludes solving process
}

void SatJob::extractResult() {
    result.id = getDescription().getId();
    result.result = resultCode;
    result.solution.clear();
    if (resultCode == SAT) {
        result.solution = solver->getTruthValues();
    } else if (resultCode == UNSAT) {
        std::set<int>& assumptions = solver->getFailedAssumptions();
        std::copy(assumptions.begin(), assumptions.end(), std::back_inserter(result.solution));
    }
}

void SatJob::beginCommunication() {

    JobMessage msg;
    msg.payload = collectClausesFromSolvers();
    if (isRoot()) {
        // There are no other nodes computing on this job
        //Console::log("Self-broadcasting clauses");
        learnClausesFromAbove(msg.payload);
        return;
    }
    msg.jobId = jobId;
    msg.epoch = epochCounter.getEpoch();
    msg.tag = MSG_GATHER_CLAUSES;
    int parentRank = getParentNodeRank();
    Console::log_send(Console::VERB, parentRank, "Sending clauses of effective size %i from %s", msg.payload.size(), toStr());
    MyMpi::isend(MPI_COMM_WORLD, parentRank, MSG_JOB_COMMUNICATION, msg);
}

void SatJob::communicate(int source, JobMessage& msg) {

    if (isNotInState({JobState::ACTIVE}))
        return;

    int jobId = msg.jobId;
    int epoch = msg.epoch;
    std::vector<int>& clauses = msg.payload;

    if (epoch != (int)epochCounter.getEpoch()) {
        Console::log(Console::VERB, "Discarding job message from a previous epoch.");
        return;
    }

    if (msg.tag == MSG_GATHER_CLAUSES) {

        Console::log(Console::VERB, "%s received clauses from below of effective size %i", toStr(), clauses.size());

        collectClausesFromBelow(clauses);

        if (canShareCollectedClauses()) {
            std::vector<int> clausesToShare = shareCollectedClauses();
            if (isRoot()) {
                Console::log(Console::VERB, "Switching clause exchange from gather to broadcast");
                learnAndDistributeClausesDownwards(clausesToShare);
            } else {
                int parentRank = getParentNodeRank();
                JobMessage msg;
                msg.jobId = jobId;
                msg.epoch = epochCounter.getEpoch();
                msg.tag = MSG_GATHER_CLAUSES;
                Console::log_send(Console::VERB, parentRank, "Gathering clauses upwards from %s", toStr());
                MyMpi::isend(MPI_COMM_WORLD, parentRank, MSG_JOB_COMMUNICATION, msg);
            }
        }

    } else if (msg.tag == MSG_DISTRIBUTE_CLAUSES) {
        learnAndDistributeClausesDownwards(clauses);
    }
}

void SatJob::learnAndDistributeClausesDownwards(std::vector<int>& clauses) {

    Console::log(Console::VVERB, "%s received %i broadcast clauses", toStr(), clauses.size());
    assert(clauses.size() % BROADCAST_CLAUSE_INTS_PER_NODE == 0);

    learnClausesFromAbove(clauses);

    JobMessage msg;
    msg.jobId = jobId;
    msg.epoch = epochCounter.getEpoch();
    msg.tag = MSG_DISTRIBUTE_CLAUSES;
    msg.payload = clauses;
    int childRank;
    if (hasLeftChild()) {
        childRank = getLeftChildNodeRank();
        Console::log_send(Console::VERB, childRank, "%s broadcasting clauses downwards", toStr());
        MyMpi::isend(MPI_COMM_WORLD, childRank, MSG_JOB_COMMUNICATION, msg);
    }
    if (hasRightChild()) {
        childRank = getRightChildNodeRank();
        Console::log_send(Console::VERB, childRank, "%s broadcasting clauses downwards", toStr());
        MyMpi::isend(MPI_COMM_WORLD, childRank, MSG_JOB_COMMUNICATION, msg);
    }
}

std::vector<int> SatJob::collectClausesFromSolvers() {
    if (!solver->isFullyInitialized()) {
        return std::vector<int>(BROADCAST_CLAUSE_INTS_PER_NODE, 0);
    }
    return solver->prepareSharing( commSize /*job.getVolume()*/);
}
void SatJob::insertIntoClauseBuffer(std::vector<int>& vec) {

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
void SatJob::collectClausesFromBelow(std::vector<int>& clauses) {

    insertIntoClauseBuffer(clauses);
    sharedClauseSources++;
}
bool SatJob::canShareCollectedClauses() {

    int numChildren = 0;
    // Must have received clauses from both children,
    // except if one / both of them cannot exist according to volume
    if (hasLeftChild()) numChildren++;
    if (hasRightChild()) numChildren++;
    return numChildren == sharedClauseSources;
}
std::vector<int> SatJob::shareCollectedClauses() {

    std::vector<int> selfClauses = collectClausesFromSolvers();
    insertIntoClauseBuffer(selfClauses);
    std::vector<int> vec = clausesToShare;
    sharedClauseSources = 0;
    clausesToShare.resize(0);
    return vec;
}
void SatJob::learnClausesFromAbove(std::vector<int>& clauses) {
    if (!solver->isFullyInitialized())
        return; // discard clauses TODO keep?
    Console::log(Console::VERB, "Digesting clauses ...");
    solver->digestSharing(clauses);
    Console::log(Console::VERB, "Digested clauses.");
}

int SatJob::solveLoop() {

    int result = -1;

    // Already reported the actual result, or still initializing
    if (doneLocally) {
        return result;
    }

    if (isInState({ACTIVE})) {
        // if result is found, stops all solvers
        // but does not call finishSolving()
        result = solver->solveLoop();

    } else if (isInState({INITIALIZING_TO_PAST, INITIALIZING_TO_SUSPENDED, INITIALIZING_TO_ACTIVE})) {
        if (solver == NULL || !solver->isRunning() || !solver->isFullyInitialized())
            return result;
        JobState oldState = state;
        switchState(ACTIVE);
        if (oldState == INITIALIZING_TO_PAST) {
            terminate();
        } else if (oldState == INITIALIZING_TO_SUSPENDED) {
            suspend();
        }
        return result;
    }

    if (result >= 0) {
        doneLocally = true;
        this->resultCode = result;
        Console::log_send(Console::INFO, getRootNodeRank(), "%s found result %s", toStr(), 
                            result == 10 ? "SAT" : result == 20 ? "UNSAT" : "UNKNOWN");
        extractResult();
    }
    return result;
}