
#include <map>
#include <thread>

#include "assert.h"
#include "util/console.h"
#include "util/timer.h"
#include "util/mympi.h"
#include "data/sat_job.h"
#include "util/console_horde_interface.h"

void SatJob::lockHordeManipulation() {
    _horde_manipulation_lock.lock();
}
void SatJob::unlockHordeManipulation() {
    _horde_manipulation_lock.unlock();
}

void SatJob::appl_initialize() {

    assert(_has_description);

    // Initialize Hordesat instance
    assert(_solver == NULL || Console::fail("Solver is not NULL! State of %s : %s", toStr(), jobStateToStr()));
    Console::log(Console::VERB, "%s : preparing params", toStr());
    std::map<std::string, std::string> params;
    params["e"] = "1"; // exchange mode: 0 = nothing, 1 = alltoall, 2 = log, 3 = asyncrumor
    params["c"] = this->_params.getParam("t"); // solver threads on this node
    if (_params.getIntParam("md") <= 1 && _params.getIntParam("t") <= 1) {
        // One thread on one node: do not diversify anything, but keep default solver settings
        params["d"] = "0"; // no diversification
    } else {
        params["d"] = "7"; // sparse random + native diversification
    }
    params["i"] = "0"; // #microseconds to sleep during solve loop
    params["v"] = (this->_params.getIntParam("v") >= 3 ? "1" : "0"); // verbosity
    params["mpirank"] = std::to_string(_index); // mpi_rank
    params["mpisize"] = std::to_string(_comm_size); // mpi_size
    std::string identifier = std::string(toStr());
    params["jobstr"] = identifier;

    if (_abort_after_initialization) {
        endInitialization();
        appl_withdraw();
        return;
    }

    _horde_manipulation_lock = VerboseMutex((std::string("HordeManip") + toStr()).c_str());
    lockHordeManipulation();
    Console::log(Console::VERB, "%s : creating horde instance", toStr());
    _solver = std::unique_ptr<HordeLib>(new HordeLib(params, std::shared_ptr<LoggingInterface>(new ConsoleHordeInterface(identifier))));
    unlockHordeManipulation();

    if (_abort_after_initialization) {
        endInitialization();
        appl_withdraw();
        return;
    }

    lockHordeManipulation();
    if (_solver != NULL) {
        Console::log(Console::VERB, "%s : beginning to solve", toStr());
        _solver->beginSolving(_description.getPayloads(), _description.getAssumptions(_description.getRevision()));
        Console::log(Console::VERB, "%s : finished HordeLib instance initialization", toStr());
    }
    unlockHordeManipulation();

    if (_abort_after_initialization) {
        endInitialization();
        appl_withdraw();
    }
}

void SatJob::appl_updateRole() {
    if (_solver != NULL) _solver->diversify(_index, _comm_size);
}

void SatJob::appl_updateDescription(int fromRevision) {
    lockHordeManipulation();
    std::vector<VecPtr> formulaAmendments = _description.getPayloads(fromRevision, _description.getRevision());
    _done_locally = false;
    _solver->continueSolving(formulaAmendments, _description.getAssumptions(_description.getRevision()));
    unlockHordeManipulation();
}

void SatJob::appl_pause() {
    _solver->setPaused();
}

void SatJob::appl_unpause() {
    _solver->unsetPaused();
}

void SatJob::appl_interrupt() {
    lockHordeManipulation();
    if (_solver != NULL) {
        _solver->interrupt(); // interrupt SAT solving (but keeps solver threads!)
        _solver->finishSolving(); // concludes solving process
    }
    unlockHordeManipulation();
}

void SatJob::setSolverNull() {
    Console::log(Console::VERB, "Releasing solver ...");
    lockHordeManipulation();
    if (_solver != NULL) {
        _solver.reset();
        _solver = NULL;
        Console::log(Console::VERB, "Solver released.");
    }
    unlockHordeManipulation();
}

void SatJob::setSolverNullThread() {
    setSolverNull();
    _bg_thread_running = false;
}

void SatJob::appl_withdraw() {

    lockHordeManipulation();
    if (isInitializing()) {
        _abort_after_initialization = true;
    }
    if (_solver != NULL) {
        _solver->abort();
        unlockHordeManipulation();
        // Do cleanup of HordeLib and its threads in a separate thread to avoid blocking
        _bg_thread_running = true;
        _bg_thread = std::thread(&SatJob::setSolverNullThread, this);
    } else {
        unlockHordeManipulation();
    }
}

void SatJob::extractResult() {
    _result.id = getId();
    _result.result = _result_code;
    _result.revision = _description.getRevision();
    _result.solution.clear();
    if (_result_code == SAT) {
        _result.solution = _solver->getTruthValues();
    } else if (_result_code == UNSAT) {
        std::set<int>& assumptions = _solver->getFailedAssumptions();
        std::copy(assumptions.begin(), assumptions.end(), std::back_inserter(_result.solution));
    }
}

void SatJob::appl_beginCommunication() {

    JobMessage msg;
    if (isRoot()) {
        // There are no other nodes computing on this job:
        // internally learn collected clauses, if ACTIVE
        int jobCommEpoch = getJobCommEpoch();
        if (isInState({ACTIVE})) {
            msg.payload = collectClausesFromSolvers();
            learnClausesFromAbove(msg.payload, jobCommEpoch);
        }
        _last_shared_job_comm = jobCommEpoch;
        return;
    }
    msg.jobId = _id;
    msg.epoch = getJobCommEpoch();
    msg.tag = MSG_GATHER_CLAUSES;
    msg.payload = collectClausesFromSolvers();
    int parentRank = getParentNodeRank();
    Console::log_send(Console::VERB, parentRank, "%s : (JCE=%i) sending, size %i", toStr(), msg.epoch, msg.payload.size());
    MyMpi::isend(MPI_COMM_WORLD, parentRank, MSG_JOB_COMMUNICATION, msg);
    // TODO //stats.increase("sentMessages");
}

void SatJob::appl_communicate(int source, JobMessage& msg) {

    if (isNotInState({JobState::ACTIVE}))
        return;

    // Unpack job message
    int jobId = msg.jobId;
    int epoch = msg.epoch;
    std::vector<int>& clauses = msg.payload;

    if (msg.tag == MSG_GATHER_CLAUSES) {
        // Gather received clauses, send to parent
        
        Console::log(Console::VERB, "%s : (JCE=%i) received, size %i", toStr(), epoch, clauses.size());

        if (_last_shared_job_comm >= epoch) {
            // Already shared clauses upwards this job comm epoch!
            Console::log(Console::VERB, "%s : (JCE=%i) ending: already did sharing this JCE", toStr(), epoch);
            Console::log(Console::VERB, "%s : (JCE=%i) learning and broadcasting down", toStr(), epoch);
            learnAndDistributeClausesDownwards(clauses, epoch);
            return;
        }
        
        // Add received clauses to local set of collected clauses
        collectClausesFromBelow(clauses, epoch);

        // Ready to share the clauses?
        if (canShareCollectedClauses()) {

            std::vector<int> clausesToShare = shareCollectedClauses(epoch);
            if (isRoot()) {
                // Share complete set of clauses to children
                Console::log(Console::VERB, "%s : (JCE=%i) switching: gather => broadcast", toStr(), epoch); 
                learnAndDistributeClausesDownwards(clausesToShare, epoch);
            } else {
                // Send set of clauses to parent
                int parentRank = getParentNodeRank();
                JobMessage msg;
                msg.jobId = jobId;
                msg.epoch = epoch;
                msg.tag = MSG_GATHER_CLAUSES;
                msg.payload = clausesToShare;
                Console::log_send(Console::VERB, parentRank, "%s : (JCE=%i) gathering", toStr(), epoch);
                MyMpi::isend(MPI_COMM_WORLD, parentRank, MSG_JOB_COMMUNICATION, msg);
            }
            _last_shared_job_comm = epoch;
        }

    } else if (msg.tag == MSG_DISTRIBUTE_CLAUSES) {
        // Learn received clauses, send them to children
        learnAndDistributeClausesDownwards(clauses, epoch);
    }
}

void SatJob::learnAndDistributeClausesDownwards(std::vector<int>& clauses, int jobCommEpoch) {

    Console::log(Console::VVERB, "%s : (JCE=%i) learning, size %i", toStr(), jobCommEpoch, clauses.size());
    assert(clauses.size() % BROADCAST_CLAUSE_INTS_PER_NODE == 0);

    // Send clauses to children
    JobMessage msg;
    msg.jobId = _id;
    msg.epoch = jobCommEpoch;
    msg.tag = MSG_DISTRIBUTE_CLAUSES;
    msg.payload = clauses;
    int childRank;
    if (hasLeftChild()) {
        childRank = getLeftChildNodeRank();
        Console::log_send(Console::VERB, childRank, "%s : (JCE=%i) broadcasting", toStr(), jobCommEpoch);
        MyMpi::isend(MPI_COMM_WORLD, childRank, MSG_JOB_COMMUNICATION, msg);
    }
    if (hasRightChild()) {
        childRank = getRightChildNodeRank();
        Console::log_send(Console::VERB, childRank, "%s : (JCE=%i) broadcasting", toStr(), jobCommEpoch);
        MyMpi::isend(MPI_COMM_WORLD, childRank, MSG_JOB_COMMUNICATION, msg);
    }

    if (clauses.size() > 0) {
        // Locally learn clauses
        learnClausesFromAbove(clauses, jobCommEpoch);
    }
}

std::vector<int> SatJob::collectClausesFromSolvers() {

    // If not fully initialized yet, broadcast an empty set of clauses
    if (isNotInState({ACTIVE}) || !_solver->isFullyInitialized()) {
        return std::vector<int>(BROADCAST_CLAUSE_INTS_PER_NODE, 0);
    }
    // Else, retrieve clauses from solvers
    return _solver->prepareSharing();
}
void SatJob::insertIntoClauseBuffer(std::vector<int>& vec, int jobCommEpoch) {

    // If there are clauses in the buffer which are from a previous job comm epoch:
    if (!_clause_buffer.empty() && _job_comm_epoch_of_clause_buffer != jobCommEpoch) {
        // Previous clauses came from an old epoch; reset clause buffer
        Console::log(Console::VVERB, "(JCE=%i) Discarding buffer, size %i, from old JCE %i", 
                jobCommEpoch, _clause_buffer.size(), _job_comm_epoch_of_clause_buffer);
        _num_clause_sources = 0;
        _clause_buffer.resize(0);
    }
    // Update epoch of current clause buffer
    _job_comm_epoch_of_clause_buffer = jobCommEpoch;

    // Insert clauses into local clause buffer for later sharing
    _clause_buffer.insert(_clause_buffer.end(), vec.begin(), vec.end());

    // Resize to multiple of #clause-ints per node
    int prevSize = _clause_buffer.size();
    int remainder = prevSize % BROADCAST_CLAUSE_INTS_PER_NODE;
    if (remainder != 0) {
        _clause_buffer.resize(prevSize + BROADCAST_CLAUSE_INTS_PER_NODE-remainder);
        std::fill(_clause_buffer.begin()+prevSize, _clause_buffer.end(), 0);
    }
    assert(_clause_buffer.size() % BROADCAST_CLAUSE_INTS_PER_NODE == 0);
}
void SatJob::collectClausesFromBelow(std::vector<int>& clauses, int jobCommEpoch) {

    insertIntoClauseBuffer(clauses, jobCommEpoch);
    _num_clause_sources++;
}
bool SatJob::canShareCollectedClauses() {

    int numChildren = 0;
    // Must have received clauses from both children,
    // except if one / both of them cannot exist according to volume
    if (hasLeftChild()) numChildren++;
    if (hasRightChild()) numChildren++;
    return numChildren == _num_clause_sources;
}
std::vector<int> SatJob::shareCollectedClauses(int jobCommEpoch) {

    // Locally collect clauses from own solvers, add to clause buffer
    std::vector<int> selfClauses = collectClausesFromSolvers();
    insertIntoClauseBuffer(selfClauses, jobCommEpoch);
    std::vector<int> vec = _clause_buffer;

    // Reset clause buffer
    _num_clause_sources = 0;
    _clause_buffer.resize(0);
    return vec;
}
void SatJob::learnClausesFromAbove(std::vector<int>& clauses, int jobCommEpoch) {

    // If not active or not fully initialized yet: discard clauses
    if (isNotInState({ACTIVE}) || !_solver->isFullyInitialized()) {
        Console::log(Console::VVERB, "%s : (JCE=%i) discarded because job is not (yet?) active", 
                toStr(), jobCommEpoch);
        return;
    }

    // Locally digest clauses
    Console::log(Console::VVERB, "%s : (JCE=%i) digesting ...", toStr(), jobCommEpoch);
    lockHordeManipulation();
    if (_solver != NULL) _solver->digestSharing(clauses);
    unlockHordeManipulation();
    Console::log(Console::VVERB, "%s : (JCE=%i) digested", toStr(), jobCommEpoch);
    /*} else {
        Console::log(Console::VVERB, "%s : (JCE=%i) discarded because job is being manipulated", 
            toStr(), jobCommEpoch);
    }*/
}

int SatJob::appl_solveLoop() {

    int result = -1;

    // Already reported the actual result, or still initializing
    if (_done_locally) {
        return result;
    }

    if (isInState({ACTIVE})) {
        // If result is found here, stops all solvers
        // but does not call finishSolving()
        result = _solver->solveLoop();

    } else if (isInitializing()) {
        // Still initializing?
        if (_solver == NULL || !_solver->isFullyInitialized())
            // Yes, some stuff still is not initialized
            return result;
        // Else: end initialization
        Console::log(Console::VERB, "%s : solver threads have been fully initialized by now", toStr());
        endInitialization();

        // If initialized and active now, do solve loop
        if (isInState({ACTIVE})) {
            result = _solver->solveLoop();
        } else {
            return result;
        }
    }

    // Did a solver find a result?
    if (result >= 0) {
        _done_locally = true;
        this->_result_code = result;
        Console::log_send(Console::INFO, getRootNodeRank(), "%s : found result %s", toStr(), 
                            result == 10 ? "SAT" : result == 20 ? "UNSAT" : "UNKNOWN");
        extractResult();
    }
    return result;
}

void SatJob::appl_dumpStats() {
    if (isInState({ACTIVE})) {
        _solver->dumpStats();
    }
}

SatJob::~SatJob() {

    if (_bg_thread.joinable()) {
        Console::log(Console::VVERB, "%s : joining bg thread", toStr());
        _bg_thread.join(); // if already aborting
    }

    if (_solver != NULL) {
        Console::log(Console::VVERB, "%s : interrupting and deleting hordesat", toStr());
        appl_interrupt();
        _solver->abort();
        setSolverNull();
    }
    Console::log(Console::VVERB, "%s : leaving SAT job destructor", toStr());
}