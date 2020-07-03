
#include <map>
#include <thread>
#include "assert.h"

#include "util/console.hpp"
#include "util/sys/timer.hpp"
#include "comm/mympi.hpp"
#include "console_horde_interface.hpp"
#include "forked_sat_job.hpp"
#include "anytime_sat_clause_communicator.hpp"
#include "horde_shared_memory.hpp"
#include "util/sys/proc.hpp"
#include "util/sys/fork.hpp"

ForkedSatJob::ForkedSatJob(Parameters& params, int commSize, int worldRank, int jobId) : 
        BaseSatJob(params, commSize, worldRank, jobId), _job_comm_period(params.getFloatParam("s")) {
}

bool ForkedSatJob::appl_initialize() {

    assert(hasJobDescription());

    Parameters slvParams(_params);
    slvParams.setParam("mpirank", std::to_string(getIndex()));
    slvParams.setParam("mpisize", std::to_string(_comm_size));
    slvParams.setParam("rank", std::to_string(_world_rank));
    slvParams.setParam("jobid", std::to_string(getId()));
    slvParams.setParam("jobstr", std::string(toStr()));

    _solver.reset(new HordeProcessAdapter(slvParams, 
            getDescription().getPayloads(), 
            getDescription().getAssumptions(getDescription().getRevision())));
    _solver->run();

    _clause_comm = (void*) new AnytimeSatClauseCommunicator(_params, this);

    if (_abort_after_initialization) {
        return false;
    }

    if (solverNotNull()) {
        Console::log(Console::VVVERB, "%s : beginning to solve", toStr());
        _solver_pid = _solver->run();
        Console::log(Console::VERB, "%s : spawned child pid=%i", toStr(), _solver_pid);
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
    if (_solver->check()) {
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
    if (isInStateUnsafe({ACTIVE})) getSolver()->dumpStats();
}

bool ForkedSatJob::appl_isDestructible() {
    // Solver is NULL or child process terminated
    return _solver_pid == -1 || Fork::didChildExit(_solver_pid);
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

    Console::log(Console::VVVVERB, "%s : destroy clause comm", toStr());
    if (_clause_comm != NULL) {
        delete (AnytimeSatClauseCommunicator*)_clause_comm;
        _clause_comm = NULL;
    }

    Console::log(Console::VVVVERB, "%s : destroy solver env", toStr());
    if (solverNotNull()) {
        _solver = NULL;
    }
    if (_solver_pid != -1 && !Fork::didChildExit(_solver_pid)) {
        Console::log(Console::VVVVERB, "%s : SIGKILLing child pid=%i", toStr(), _solver_pid);
        Fork::hardkill(_solver_pid);
    }

    Console::log(Console::VVERB, "%s : destructed SAT job", toStr());
}