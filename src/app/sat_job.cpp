
#include <map>
#include <thread>

#include "assert.h"
#include "util/console.h"
#include "util/timer.h"
#include "util/mympi.h"
#include "util/console_horde_interface.h"
#include "app/sat_job.h"
#include "app/sat_clause_communicator.h"

void logMutexSat(const char* msg) {
    Console::log(Console::VVVERB, msg);
}

void SatJob::lockHordeManipulation() {
    _horde_manipulation_lock.lock();
}
void SatJob::unlockHordeManipulation() {
    _horde_manipulation_lock.unlock();
}

void SatJob::appl_initialize() {

    assert(hasJobDescription());

    // Initialize Hordesat instance
    assert(_solver == NULL || Console::fail("Solver is not NULL! State of %s : %s", toStr(), jobStateToStr()));
    Console::log(Console::VERB, "%s : preparing params", toStr());
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
    params["i"] = "0"; // #microseconds to sleep during solve loop
    params["v"] = (this->_params.getIntParam("v") >= 3 ? "1" : "0"); // verbosity
    params["mpirank"] = std::to_string(getIndex()); // mpi_rank
    params["mpisize"] = std::to_string(_comm_size); // mpi_size
    std::string identifier = std::string(toStr());
    params["jobstr"] = identifier;

    if (mustAbortInitialization()) return;

    _horde_manipulation_lock = VerboseMutex(std::string("HordeManip") + toStr(), &logMutexSat);
    lockHordeManipulation();
    Console::log(Console::VERB, "%s : creating horde instance", toStr());
    _solver = std::unique_ptr<HordeLib>(new HordeLib(params, std::shared_ptr<LoggingInterface>(new ConsoleHordeInterface(identifier))));
    _clause_comm = (void*) new SatClauseCommunicator(_params, this);
    unlockHordeManipulation();

    if (mustAbortInitialization()) return;

    lockHordeManipulation();
    if (_solver != NULL) {
        Console::log(Console::VERB, "%s : beginning to solve", toStr());
        JobDescription& desc = getDescription();
        _solver->beginSolving(desc.getPayloads(), desc.getAssumptions(desc.getRevision()));
        Console::log(Console::VERB, "%s : finished HordeLib instance initialization", toStr());
    }
    unlockHordeManipulation();

    if (mustAbortInitialization()) return;
}

void SatJob::appl_updateRole() {
    if (_solver != NULL) _solver->updateRole(getIndex(), _comm_size);
}

void SatJob::appl_updateDescription(int fromRevision) {
    lockHordeManipulation();
    JobDescription& desc = getDescription();
    std::vector<VecPtr> formulaAmendments = desc.getPayloads(fromRevision, desc.getRevision());
    _done_locally = false;
    _solver->continueSolving(formulaAmendments, desc.getAssumptions(desc.getRevision()));
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
    _result.revision = getDescription().getRevision();
    _result.solution.clear();
    if (_result_code == SAT) {
        _result.solution = _solver->getTruthValues();
    } else if (_result_code == UNSAT) {
        std::set<int>& assumptions = _solver->getFailedAssumptions();
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

void SatJob::appl_beginCommunication() {
    if (_clause_comm != NULL)
        ((SatClauseCommunicator*) _clause_comm)->initiateCommunication();
}

void SatJob::appl_communicate(int source, JobMessage& msg) {
    if (_clause_comm != NULL)
        ((SatClauseCommunicator*) _clause_comm)->continueCommunication(source, msg);
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