
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
#include "horde_config.hpp"

ForkedSatJob::ForkedSatJob(const Parameters& params, int commSize, int worldRank, int jobId) : 
        BaseSatJob(params, commSize, worldRank, jobId), _job_comm_period(params.getFloatParam("s")) {
}

void ForkedSatJob::appl_start() {

    if (_initialized) {
        
        // Already initialized => Has a valid solver instance
        auto lock = _solver_lock.getLock();
        _done_locally = false;
        // TODO Update job index etc. from JobTree
        // TODO Update job description and amendments (in a separate thread!)
        // Continue solving
        _solver->setSolvingState(SolvingStates::ACTIVE);
    
    } else if (!_init_thread.joinable()) _init_thread = std::thread([this]() {
        
        Parameters hParams(_params);
        HordeConfig::applyDefault(hParams, *this);

        _solver.reset(new HordeProcessAdapter(hParams,
                getDescription().getPayloads(), 
                getDescription().getAssumptions(getRevision())));
        _clause_comm = (void*) new AnytimeSatClauseCommunicator(hParams, this);

        Console::log(Console::VVVERB, "%s : beginning to solve", toStr());
        _solver_pid = _solver->run();
        Console::log(Console::VERB, "%s : spawned child pid=%i", toStr(), _solver_pid);
        Console::log(Console::VERB, "%s : finished horde initialization", toStr());
        _time_of_start_solving = Timer::elapsedSeconds();

        _initialized = true;

        auto lock = _solver_lock.getLock();
        auto state = getState();
        if (state == SUSPENDED) _solver->setSolvingState(SolvingStates::SUSPENDED);
        if (state == INACTIVE || state == PAST) _solver->setSolvingState(SolvingStates::STANDBY);
        if (state == PAST) {
            _solver->setSolvingState(SolvingStates::ABORTING);
            delete (AnytimeSatClauseCommunicator*)_clause_comm;
            _clause_comm = NULL;
        }
    });
}

/*
bool ForkedSatJob::appl_doneInitializing() {
    return _solver != NULL && getSolver()->isFullyInitialized();
}

void ForkedSatJob::appl_updateRole() {
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
*/

void ForkedSatJob::appl_suspend() {
    if (!_initialized) return;
    auto lock = _solver_lock.getLock();
    _solver->setSolvingState(SolvingStates::SUSPENDED);
}

void ForkedSatJob::appl_resume() {
    if (!_initialized) return;
    auto lock = _solver_lock.getLock();
    _solver->setSolvingState(SolvingStates::ACTIVE);
}

void ForkedSatJob::appl_stop() {
    if (!_initialized) return;
    auto lock = _solver_lock.getLock();
    _solver->setSolvingState(SolvingStates::STANDBY);
}

void ForkedSatJob::appl_terminate() {
    if (!_initialized) return;
    auto lock = _solver_lock.getLock();
    delete (AnytimeSatClauseCommunicator*)_clause_comm;
    _clause_comm = NULL;
    _solver->setSolvingState(SolvingStates::ABORTING);
}

int ForkedSatJob::appl_solved() {

    int result = -1;
    if (!_initialized || getState() != ACTIVE) return result;
    if (_done_locally) return result;

    // Did a solver find a result?
    auto lock = _solver_lock.getLock();
    if (_solver->check()) {
        auto solution = _solver->getSolution();
        result = solution.first;
        Console::log_send(Console::INFO, getJobTree().getRootNodeRank(), "%s : found result %s", toStr(), 
                            result == 10 ? "SAT" : result == 20 ? "UNSAT" : "UNKNOWN");
        _internal_result.id = getId();
        _internal_result.result = result;
        _internal_result.revision = getRevision();
        _internal_result.solution = solution.second;
        _done_locally = true;
    }
    return result;
}

JobResult ForkedSatJob::appl_getResult() {
    return _internal_result;
}

void ForkedSatJob::appl_dumpStats() {
    if (!_initialized || getState() != ACTIVE) return;
    _solver->dumpStats();
}

bool ForkedSatJob::appl_isDestructible() {
    // Solver is NULL or child process terminated
    return !_initialized || Fork::didChildExit(_solver_pid);
}

bool ForkedSatJob::appl_wantsToBeginCommunication() {
    if (!_initialized || getState() != ACTIVE || _job_comm_period <= 0) return false;
    // Special "timed" conditions for leaf nodes:
    if (getJobTree().isLeaf()) {
        // At least half a second since initialization / reactivation
        if (getAgeSinceActivation() < 0.5 * _job_comm_period) return false;
        // At least params["s"] seconds since last communication 
        if (Timer::elapsedSeconds()-_time_of_last_comm < _job_comm_period) return false;
    }
    if (!_solver_lock.tryLock()) return false;
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
    if (getJobTree().isLeaf()) _time_of_last_comm = Timer::elapsedSeconds();
}

void ForkedSatJob::appl_communicate(int source, JobMessage& msg) {
    Console::log(Console::VVVVERB, "comm");
    if (_clause_comm == NULL) return;
    auto lock = _solver_lock.getLock();
    if (_clause_comm != NULL)
        ((AnytimeSatClauseCommunicator*) _clause_comm)->handle(source, msg);
}

bool ForkedSatJob::isInitialized() {
    return _initialized && _solver->isFullyInitialized();
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
    if (_init_thread.joinable()) _init_thread.join();
    _solver = NULL;
    if (_solver_pid != -1 && !Fork::didChildExit(_solver_pid)) {
        Console::log(Console::VVVVERB, "%s : SIGKILLing child pid=%i", toStr(), _solver_pid);
        Fork::hardkill(_solver_pid);
    }
    Console::log(Console::VVERB, "%s : destructed SAT job", toStr());
}