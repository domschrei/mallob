
#include <map>
#include <thread>

#include "assert.h"
#include "util/console.h"
#include "util/timer.h"
#include "util/mympi.h"
#include "util/console_horde_interface.h"
#include "app/forked_sat_job.h"
#include "app/anytime_sat_clause_communicator.h"
#include "util/memusage.h"
#include "util/fork.h"

ForkedSatJob::ForkedSatJob(Parameters& params, int commSize, int worldRank, int jobId, EpochCounter& epochCounter) : 
        BaseSatJob(params, commSize, worldRank, jobId, epochCounter), _job_comm_period(params.getFloatParam("s")) {
}

bool ForkedSatJob::appl_initialize() {

    assert(hasJobDescription());
    //_horde_manipulation_lock.updateName(std::string("HordeManip") + toStr());

    // Initialize Hordesat instance
    assert(_solver == NULL || Console::fail("Solver is not NULL! State of %s : %s", toStr(), jobStateToStr()));
    Console::log(Console::VVVERB, "%s : preparing params", toStr());
    std::map<std::string, std::string> params;
    params["e"] = "1"; // exchange mode: 0 = nothing, 1 = alltoall, 2 = log, 3 = asyncrumor
    params["c"] = this->_params.getParam("t"); // solver threads on this node
    if (_params.getIntParam("md") <= 1 && _params.getIntParam("t") <= 1) {
        // One thread on one node: do not diversify anything, but keep default solver settings
        params["d"] = "0"; // no diversification
    } else if (this->_params.isSet("nophase")) {
        // Do not do sparse random ("phase") diversification
        params["d"] = "4"; // native diversification only
    } else {
        params["d"] = "7"; // sparse random + native diversification
    }
    params["fd"]; // filter duplicate clauses
    params["cbbs"] = _params.getParam("cbbs"); // clause buffer base size
    params["icpr"] = _params.getParam("icpr"); // increase clause production
    params["mcl"] = _params.getParam("mcl"); // max clause length
    params["i"] = "0"; // #microseconds to sleep during solve loop
    params["v"] = 99; // (this->_params.getIntParam("v") >= 3 ? "1" : "0"); // verbosity
    params["mpirank"] = std::to_string(getIndex()); // mpi_rank
    params["mpisize"] = std::to_string(_comm_size); // mpi_size
    std::string identifier = std::string(toStr());
    params["jobstr"] = identifier;
    if (_params.isSet("sinst")) {
        // Single instance filename
        params["sinst"] = _params.getParam("sinst");
    }
    params["cfhl"] = _params.getParam("cfhl");
    if (_params.isSet("aod")) params["aod"];

    if (_abort_after_initialization) {
        return false;
    }

    auto lock = _solver_lock.getLock();
    Console::log(Console::VERB, "%s : creating horde instance", toStr());
    _solver = std::unique_ptr<HordeProcessAdapter>(new HordeProcessAdapter(params, std::shared_ptr<LoggingInterface>(new ConsoleHordeInterface("<h-" + identifier + ">")), 
                getDescription().getPayloads(), getDescription().getAssumptions(getDescription().getRevision())));
    _clause_comm = (void*) new AnytimeSatClauseCommunicator(_params, this);

    if (_abort_after_initialization) {
        return false;
    }

    if (solverNotNull()) {
        Console::log(Console::VVVERB, "%s : beginning to solve", toStr());
        _solver->run();
        Console::log(Console::VERB, "%s : finished horde initialization", toStr());
        _time_of_start_solving = Timer::elapsedSeconds();
    }

    return !_abort_after_initialization;
}

bool ForkedSatJob::appl_doneInitializing() {
    return _solver != NULL && getSolver()->isFullyInitialized();
}

void ForkedSatJob::appl_updateRole() {
    if (!solverNotNull()) return;
    auto lock = _solver_lock.getLock();
    if (solverNotNull()) getSolver()->updateRole(getIndex(), _comm_size);
}

void ForkedSatJob::appl_updateDescription(int fromRevision) {
    auto lock = _solver_lock.getLock();
    JobDescription& desc = getDescription();
    std::vector<VecPtr> formulaAmendments = desc.getPayloads(fromRevision, desc.getRevision());
    assert(Console::fail("Not implemented yet!"));
    //if (solverNotNull()) getSolver()->continueSolving(formulaAmendments, desc.getAssumptions(desc.getRevision()));
}

void ForkedSatJob::appl_pause() {
    if (!solverNotNull()) return;
    auto lock = _solver_lock.getLock();
    if (solverNotNull()) getSolver()->setSolvingState(SolvingStates::SUSPENDED);
}

void ForkedSatJob::appl_unpause() {
    if (!solverNotNull()) return;
    auto lock = _solver_lock.getLock();
    if (solverNotNull()) getSolver()->setSolvingState(SolvingStates::ACTIVE);
}

void ForkedSatJob::appl_interrupt() {
    if (!solverNotNull()) return;
    auto lock = _solver_lock.getLock();
    if (solverNotNull()) {
        _solver->setSolvingState(SolvingStates::STANDBY); // interrupt SAT solving (but keeps solver threads!)
        _solver->dumpStats(); // concludes solving process
    }
}

void ForkedSatJob::appl_withdraw() {

    auto lock = _solver_lock.getLock();
    if (isInitializingUnsafe()) {
        _abort_after_initialization = true;
    }
    if (_clause_comm != NULL) {
        delete (AnytimeSatClauseCommunicator*)_clause_comm;
        _clause_comm = NULL;
    }
    if (solverNotNull()) {
        getSolver()->setSolvingState(SolvingStates::ABORTING);
    }
}

int ForkedSatJob::appl_solveLoop() {

    int result = -1;
    if (getState() != ACTIVE) return result;
    if (_done_locally) return result;

    // Did a solver find a result?
    if (_solver->hasSolution()) {
        auto solution = _solver->getSolution();
        result = solution.first;
        Console::log_send(Console::INFO, getRootNodeRank(), "%s : found result %s", toStr(), 
                            result == 10 ? "SAT" : result == 20 ? "UNSAT" : "UNKNOWN");
        auto lock = _solver_lock.getLock();
        _result.id = getId();
        _result.result = result;
        _result.revision = getDescription().getRevision();
        _result.solution = solution.second;
        _done_locally = true;
    }
    return result;
}

void ForkedSatJob::appl_dumpStats() {
    if (isInState({ACTIVE})) {
        getSolver()->dumpStats();
    }
}

bool ForkedSatJob::appl_isDestructible() {
    // Solver is NULL or child process terminated
    return !solverNotNull() || Fork::allChildrenSignalsArrived();
}

bool ForkedSatJob::appl_wantsToBeginCommunication() const {
    if (_job_comm_period <= 0) return false;
    if (_clause_comm == NULL) return false;
    // Special "timed" conditions for leaf nodes:
    if (isLeaf()) {
        // At least half a second since initialization / reactivation
        if (getAgeSinceActivation() < 0.5 * _job_comm_period) return false;
        // At least params["s"] seconds since last communication 
        if (Timer::elapsedSeconds()-_time_of_last_comm < _job_comm_period) return false;
    }
    bool locked = _solver_lock.tryLock();
    if (!locked) {
        Console::log(Console::VVVVERB, "cannot lock: no comm");    
        return false;
    } 
    bool wants = ((AnytimeSatClauseCommunicator*) _clause_comm)->canSendClauses();
    _solver_lock.unlock();
    return wants;
}

void ForkedSatJob::appl_beginCommunication() {
    Console::log(Console::VVVVERB, "begincomm");
    if (_clause_comm == NULL) return;
    auto lock = _solver_lock.getLock();
    if (_clause_comm != NULL) 
        ((AnytimeSatClauseCommunicator*) _clause_comm)->sendClausesToParent();
    if (isLeaf()) _time_of_last_comm = Timer::elapsedSeconds();
}

void ForkedSatJob::appl_communicate(int source, JobMessage& msg) {
    Console::log(Console::VVVVERB, "comm");
    if (_clause_comm == NULL) return;
    auto lock = _solver_lock.getLock();
    if (_clause_comm != NULL)
        ((AnytimeSatClauseCommunicator*) _clause_comm)->handle(source, msg);
}

bool ForkedSatJob::isInitialized() {
    if (!solverNotNull()) return false;
    return _solver->isFullyInitialized();
}
void ForkedSatJob::prepareSharing(int maxSize) {
    _solver->collectClauses(maxSize);
}
bool ForkedSatJob::hasPreparedSharing() {
    return _solver->hasCollectedClauses();
}
std::vector<int> ForkedSatJob::getPreparedClauses() {
    return _solver->getCollectedClauses();
}
void ForkedSatJob::digestSharing(const std::vector<int>& clauses) {
    _solver->digestClauses(clauses);
}

ForkedSatJob::~ForkedSatJob() {

    Console::log(Console::VVERB, "%s : enter destructor", toStr());
    auto lock = _solver_lock.getLock();

    if (_clause_comm != NULL) {
        delete (AnytimeSatClauseCommunicator*)_clause_comm;
        _clause_comm = NULL;
    }

    if (solverNotNull()) {
        _solver = NULL;
    }

    Console::log(Console::VVERB, "%s : destructed SAT job", toStr());
}