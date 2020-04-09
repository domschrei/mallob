
#include <map>
#include <thread>

#include "assert.h"
#include "util/console.h"
#include "util/timer.h"
#include "util/mympi.h"
#include "util/console_horde_interface.h"
#include "app/sat_job.h"
#include "app/sat_clause_communicator.h"
#include "util/memusage.h"

SatJob::SatJob(Parameters& params, int commSize, int worldRank, int jobId, EpochCounter& epochCounter) : 
        Job(params, commSize, worldRank, jobId, epochCounter), _done_locally(false) {
}

void SatJob::lockHordeManipulation() {
    _horde_manipulation_lock.lock();
}
void SatJob::unlockHordeManipulation() {
    _horde_manipulation_lock.unlock();
}

bool SatJob::appl_initialize() {

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
    params["i"] = "0"; // #microseconds to sleep during solve loop
    params["v"] = 99; // (this->_params.getIntParam("v") >= 3 ? "1" : "0"); // verbosity
    params["mpirank"] = std::to_string(getIndex()); // mpi_rank
    params["mpisize"] = std::to_string(_comm_size); // mpi_size
    std::string identifier = std::string(toStr());
    params["jobstr"] = identifier;

    auto lock = _horde_manipulation_lock.getLock();

    if (_abort_after_initialization) {
        return false;
    }

    Console::log(Console::VERB, "%s : creating horde instance", toStr());
    _solver = std::unique_ptr<HordeLib>(new HordeLib(params, std::shared_ptr<LoggingInterface>(new ConsoleHordeInterface(identifier))));
    _clause_comm = (void*) new SatClauseCommunicator(_params, this);

    if (_abort_after_initialization) {
        return false;
    }

    if (solverNotNull()) {
        Console::log(Console::VVVERB, "%s : beginning to solve", toStr());
        JobDescription& desc = getDescription();
        getSolver()->beginSolving(desc.getPayloads(), desc.getAssumptions(desc.getRevision()));
        Console::log(Console::VERB, "%s : finished horde initialization", toStr());
        _time_of_start_solving = Timer::elapsedSeconds();
    }

    return !_abort_after_initialization;
}

void SatJob::appl_updateRole() {
    if (!solverNotNull()) return;
    auto lock = _horde_manipulation_lock.getLock();
    if (solverNotNull()) getSolver()->updateRole(getIndex(), _comm_size);
}

void SatJob::appl_updateDescription(int fromRevision) {
    auto lock = _horde_manipulation_lock.getLock();
    JobDescription& desc = getDescription();
    std::vector<VecPtr> formulaAmendments = desc.getPayloads(fromRevision, desc.getRevision());
    _done_locally = false;
    if (solverNotNull()) getSolver()->continueSolving(formulaAmendments, desc.getAssumptions(desc.getRevision()));
}

void SatJob::appl_pause() {
    if (!solverNotNull()) return;
    auto lock = _horde_manipulation_lock.getLock();
    if (solverNotNull()) getSolver()->setPaused();
}

void SatJob::appl_unpause() {
    if (!solverNotNull()) return;
    auto lock = _horde_manipulation_lock.getLock();
    if (solverNotNull()) getSolver()->unsetPaused();
}

void SatJob::appl_interrupt() {
    if (!solverNotNull()) return;
    auto lock = _horde_manipulation_lock.getLock();
    appl_interrupt_unsafe();
}

void SatJob::appl_interrupt_unsafe() {
    if (solverNotNull()) {
        _solver->interrupt(); // interrupt SAT solving (but keeps solver threads!)
        _solver->finishSolving(); // concludes solving process
    }
}

void SatJob::setSolverNull() {
    Console::log(Console::VVERB, "release solver");
    if (solverNotNull()) {
        _solver.reset();
        _solver = NULL;
        Console::log(Console::VVERB, "solver released");
    }
}

void SatJob::setSolverNullThread() {
    auto lock = _horde_manipulation_lock.getLock();
    setSolverNull();
    Console::log(Console::VVERB, "cleanup thread done");
}

void SatJob::appl_withdraw() {

    auto lock = _horde_manipulation_lock.getLock();
    if (isInitializingUnsafe()) {
        _abort_after_initialization = true;
    }
    if (_clause_comm != NULL) {
        delete (SatClauseCommunicator*)_clause_comm;
        _clause_comm = NULL;
    }
    if (solverNotNull()) {
        getSolver()->abort();
        // Do cleanup of HordeLib and its threads in a separate thread to avoid blocking
        _bg_thread = std::thread(&SatJob::setSolverNullThread, this);
    }
}

void SatJob::extractResult(int resultCode) {
    auto lock = _horde_manipulation_lock.getLock();
    _result.id = getId();
    _result.result = resultCode;
    _result.revision = getDescription().getRevision();
    _result.solution.clear();
    if (resultCode == SAT) {
        _result.solution = getSolver()->getTruthValues();
    } else if (resultCode == UNSAT) {
        std::set<int>& assumptions = getSolver()->getFailedAssumptions();
        std::copy(assumptions.begin(), assumptions.end(), std::back_inserter(_result.solution));
    }
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
        result = getSolver()->solveLoop();

    } else if (isInitializing()) {
        // Still initializing?
        if (_solver == NULL || !getSolver()->isFullyInitialized())
            // Yes, some stuff still is not initialized
            return result;
        // Else: end initialization
        Console::log(Console::VERB, "%s : threads initialized", toStr());
        endInitialization();

        // If initialized and active now, do solve loop
        if (isInState({ACTIVE})) {
            result = getSolver()->solveLoop();
        } else {
            return result;
        }
    }

    // Did a solver find a result?
    if (result >= 0) {
        _done_locally = true;
        Console::log_send(Console::INFO, getRootNodeRank(), "%s : found result %s", toStr(), 
                            result == 10 ? "SAT" : result == 20 ? "UNSAT" : "UNKNOWN");
        extractResult(result);
    }
    return result;
}

void SatJob::appl_dumpStats() {
    if (isInState({ACTIVE})) {

        getSolver()->dumpStats();
        if (_time_of_start_solving <= 0) return;
        float age = Timer::elapsedSeconds() - _time_of_start_solving;

        const std::vector<long>& threadTids = getSolver()->getSolverTids();
        for (int i = 0; i < threadTids.size(); i++) {
            if (threadTids[i] < 0) continue;
            double cpuRatio;
            thread_cpuratio(threadTids[i], age, cpuRatio);
            Console::log(Console::VERB, "%s td.%i : %.2f%% CPU", toStr(), threadTids[i], cpuRatio);
        }
    }
}

bool SatJob::appl_isDestructible() {
    return _solver == NULL;
}

void SatJob::appl_beginCommunication() {
    Console::log(Console::VVVVERB, "begincomm");
    if (_clause_comm == NULL) return;
    auto lock = _horde_manipulation_lock.getLock();
    if (_clause_comm != NULL) 
        ((SatClauseCommunicator*) _clause_comm)->initiateCommunication();
}

void SatJob::appl_communicate(int source, JobMessage& msg) {
    Console::log(Console::VVVVERB, "comm");
    if (_clause_comm == NULL) return;
    auto lock = _horde_manipulation_lock.getLock();
    if (_clause_comm != NULL)
        ((SatClauseCommunicator*) _clause_comm)->continueCommunication(source, msg);
}

SatJob::~SatJob() {

    Console::log(Console::VVERB, "%s : enter destructor", toStr());
    auto lock = _horde_manipulation_lock.getLock();

    if (_clause_comm != NULL) {
        delete (SatClauseCommunicator*)_clause_comm;
        _clause_comm = NULL;
    }

    if (_bg_thread.joinable()) {
        Console::log(Console::VVERB, "%s : joining bg thread", toStr());
        _bg_thread.join(); // if already aborting
    }

    if (solverNotNull()) {
        Console::log(Console::VVERB, "%s : destruct hordesat", toStr());
        appl_interrupt_unsafe();
        _solver->abort();
        setSolverNull();
    }
    Console::log(Console::VVERB, "%s : destructed SAT job", toStr());
}