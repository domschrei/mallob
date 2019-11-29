
#include <map>
#include <thread>

#include "assert.h"
#include "util/console.h"
#include "util/timer.h"
#include "util/mympi.h"
#include "data/sat_job.h"
#include "util/console_horde_interface.h"

void SatJob::initialize() {

    assert(isInitializing());
    assert(_has_description);

    // Initialize Hordesat instance
    assert(_solver == NULL || Console::fail("Solver is not NULL! State of %s : %s", toStr(), jobStateToStr()));
    Console::log(Console::VERB, "%s : preparing params", toStr());
    std::map<std::string, std::string> params;
    params["e"] = "1"; // exchange mode: 0 = nothing, 1 = alltoall, 2 = log, 3 = asyncrumor
    params["c"] = this->_params.getParam("t"); // solver threads on this node
    params["d"] = "7"; // sparse random + native diversification
    params["i"] = "0"; // #microseconds to sleep during solve loop
    params["v"] = (this->_params.getIntParam("v") >= 3 ? "1" : "0"); // verbosity
    params["mpirank"] = std::to_string(_index); // mpi_rank
    params["mpisize"] = std::to_string(_comm_size); // mpi_size
    std::string identifier = std::string(toStr());
    params["jobstr"] = identifier;
    Console::log(Console::VERB, "%s : creating horde instance", toStr());
    _solver = std::unique_ptr<HordeLib>(new HordeLib(params, std::shared_ptr<LoggingInterface>(new ConsoleHordeInterface(identifier))));
    assert(_solver != NULL);

    Console::log(Console::VERB, "%s : beginning to solve", toStr());
    _solver->beginSolving(_description.getPayloads(), _description.getAssumptions(_description.getRevision()));
    Console::log(Console::VERB, "%s : finished concurrent HordeLib instance initialization", toStr());

    if (_abort_after_initialization) {
        endInitialization();
    }
}

void SatJob::updateRole() {
    _solver->diversify(_index, _comm_size);
}

void SatJob::updateDescription(int fromRevision) {
    std::vector<VecPtr> formulaAmendments = _description.getPayloads(fromRevision, _description.getRevision());
    _done_locally = false;
    _solver->continueSolving(formulaAmendments, _description.getAssumptions(_description.getRevision()));
}

void SatJob::pause() {
    _solver->setPaused();
}

void SatJob::unpause() {
    _solver->unsetPaused();
}

void SatJob::interrupt() {
    _solver->interrupt(); // interrupt SAT solving (but keeps solver threads!)
    _solver->finishSolving(); // concludes solving process
}

void SatJob::withdraw() {
    _solver->abort();
    //_solver = NULL; // TODO 
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

void SatJob::beginCommunication() {

    JobMessage msg;
    msg.payload = collectClausesFromSolvers(); // empty if not ACTIVE
    if (isInState({ACTIVE}) && isRoot()) {
        // There are no other nodes computing on this job:
        // internally learn collected clauses
        int jobCommEpoch = getJobCommEpoch();
        learnClausesFromAbove(msg.payload, jobCommEpoch);
        _last_shared_job_comm = jobCommEpoch;
        return;
    }
    msg.jobId = _id;
    msg.epoch = getJobCommEpoch();
    msg.tag = MSG_GATHER_CLAUSES;
    int parentRank = getParentNodeRank();
    Console::log_send(Console::VERB, parentRank, "%s : (JCE=%i) Sending clauses of effective size %i", toStr(), msg.epoch, msg.payload.size());
    MyMpi::isend(MPI_COMM_WORLD, parentRank, MSG_JOB_COMMUNICATION, msg);
    // TODO //stats.increase("sentMessages");
}

void SatJob::communicate(int source, JobMessage& msg) {

    if (isNotInState({JobState::ACTIVE}))
        return;

    // Unpack job message
    int jobId = msg.jobId;
    int epoch = msg.epoch;
    std::vector<int>& clauses = msg.payload;

    if (msg.tag == MSG_GATHER_CLAUSES) {
        // Gather received clauses, send to parent
        
        Console::log(Console::VERB, "%s : (JCE=%i) received clauses from below of effective size %i", toStr(), epoch, clauses.size());

        if (_last_shared_job_comm >= epoch) {
            // Already shared clauses upwards this job comm epoch!
            Console::log(Console::VERB, "%s : (JCE=%i) ending exchange: already shared clauses this JCE", toStr(), epoch);
            Console::log(Console::VERB, "%s : (JCE=%i) learning and broadcasting clauses back", toStr(), epoch);
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
                Console::log(Console::VERB, "%s : (JCE=%i) switching clause exchange from gather to broadcast", toStr(), epoch);
                learnAndDistributeClausesDownwards(clausesToShare, epoch);
            } else {
                // Send set of clauses to parent
                int parentRank = getParentNodeRank();
                JobMessage msg;
                msg.jobId = jobId;
                msg.epoch = epoch;
                msg.tag = MSG_GATHER_CLAUSES;
                msg.payload = clausesToShare;
                Console::log_send(Console::VERB, parentRank, "%s : (JCE=%i) gathering clauses upwards", toStr(), epoch);
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

    Console::log(Console::VVERB, "%s : (JCE=%i) received clauses of size %i", toStr(), jobCommEpoch, clauses.size());
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
        Console::log_send(Console::VERB, childRank, "%s : (JCE=%i) broadcasting downwards", toStr(), jobCommEpoch);
        MyMpi::isend(MPI_COMM_WORLD, childRank, MSG_JOB_COMMUNICATION, msg);
    }
    if (hasRightChild()) {
        childRank = getRightChildNodeRank();
        Console::log_send(Console::VERB, childRank, "%s : (JCE=%i) broadcasting downwards", toStr(), jobCommEpoch);
        MyMpi::isend(MPI_COMM_WORLD, childRank, MSG_JOB_COMMUNICATION, msg);
    }

    // Locally learn clauses
    learnClausesFromAbove(clauses, jobCommEpoch);
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
        Console::log(Console::VVERB, "(JCE=%i) Discarding clause buffer of size %i from old job comm epoch %i", 
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
        Console::log(Console::VVERB, "%s : (JCE=%i) Ignoring clauses because job is not (yet?) active", 
                toStr(), jobCommEpoch);
        return;
    }

    // Locally digest clauses
    Console::log(Console::VVERB, "%s : (JCE=%i) digesting clauses ...", toStr(), jobCommEpoch);
    _solver->digestSharing(clauses);
    Console::log(Console::VVERB, "%s : (JCE=%i) digested clauses.", toStr(), jobCommEpoch);
}

int SatJob::solveLoop() {

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

void SatJob::dumpStats() {
    if (isInState({ACTIVE})) {
        _solver->dumpStats();
    }
}