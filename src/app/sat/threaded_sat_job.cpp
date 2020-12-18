
#include <map>
#include <thread>

#include "threaded_sat_job.hpp"

#include "assert.h"
#include "util/console.hpp"
#include "util/sys/timer.hpp"
#include "comm/mympi.hpp"
#include "console_horde_interface.hpp"
#include "anytime_sat_clause_communicator.hpp"
#include "util/sys/proc.hpp"
#include "horde_config.hpp"

ThreadedSatJob::ThreadedSatJob(const Parameters& params, int commSize, int worldRank, int jobId) : 
        BaseSatJob(params, commSize, worldRank, jobId), _done_locally(false), _job_comm_period(params.getFloatParam("s")) {
}

void ThreadedSatJob::appl_start() {

    if (_initialized) {
        
        // Already initialized => Has a valid solver instance
        auto lock = _solver_lock.getLock();
        _done_locally = false;
        // TODO Update job index etc. from JobTree
        // TODO Update job description and amendments (in a separate thread!)
        // Continue solving
        _solver->continueSolving(std::vector<VecPtr>(), 
                getDescription().getAssumptions(getRevision()));
    
    } else if (!_init_thread.joinable()) _init_thread = std::thread([this]() {
        
        // Initialize Hordesat instance
        Parameters hParams(_params);
        HordeConfig::applyDefault(hParams, *this);
        _solver = std::unique_ptr<HordeLib>(new HordeLib(hParams, std::shared_ptr<LoggingInterface>(new ConsoleHordeInterface(
            "<h-" + std::string(toStr()) + ">", "#" + std::to_string(getId()) + "."
        ))));
        _clause_comm = (void*) new AnytimeSatClauseCommunicator(hParams, this);

        Console::log(Console::VVVERB, "%s : beginning to solve", toStr());
        const JobDescription& desc = getDescription();
        getSolver()->beginSolving(desc.getPayloads(), desc.getAssumptions(getRevision()));
        Console::log(Console::VERB, "%s : finished horde initialization", toStr());
        _time_of_start_solving = Timer::elapsedSeconds();

        auto lock = _solver_lock.getLock();
        _initialized = true;
        auto state = getState();
        if (state == SUSPENDED) getSolver()->setPaused(); 
        if (state == INACTIVE || state == PAST) _solver->interrupt();
        if (state == PAST) terminateUnsafe();
    });
}

/*
void ThreadedSatJob::appl_updateRole() {
    if (!solverNotNull()) return;
    auto lock = _horde_manipulation_lock.getLock();
    if (solverNotNull()) getSolver()->updateRole(getIndex(), _comm_size);
}

void ThreadedSatJob::appl_updateDescription(int fromRevision) {
    auto lock = _horde_manipulation_lock.getLock();
    JobDescription& desc = getDescription();
    std::vector<VecPtr> formulaAmendments = desc.getPayloads(fromRevision, desc.getRevision());
    _done_locally = false;
    if (solverNotNull()) getSolver()->continueSolving(formulaAmendments, desc.getAssumptions(desc.getRevision()));
}
*/

void ThreadedSatJob::appl_suspend() {
    if (!_initialized) return;
    auto lock = _solver_lock.getLock();
    getSolver()->setPaused();
}

void ThreadedSatJob::appl_resume() {
    if (!_initialized) return;
    auto lock = _solver_lock.getLock();
    getSolver()->unsetPaused();
}

void ThreadedSatJob::appl_stop() {
    if (!_initialized) return;
    auto lock = _solver_lock.getLock();
    _solver->interrupt();
}

void ThreadedSatJob::appl_terminate() {
    if (!_initialized) return;
    auto lock = _solver_lock.getLock();
    terminateUnsafe();
}

void ThreadedSatJob::terminateUnsafe() {
    if (!_destroy_thread.joinable()) _destroy_thread = std::thread([this]() {
        auto lock = _solver_lock.getLock();
        delete (AnytimeSatClauseCommunicator*)_clause_comm;
        _clause_comm = NULL;
        _solver->abort();
        _solver->cleanUp();
    });
}

JobResult ThreadedSatJob::appl_getResult() {
    auto lock = _solver_lock.getLock();
    JobResult _result;
    _result.id = getId();
    _result.result = _result_code;
    _result.revision = getRevision();
    _result.solution.clear();
    if (_result_code == SAT) {
        _result.solution = getSolver()->getTruthValues();
    } else if (_result_code == UNSAT) {
        std::set<int>& assumptions = getSolver()->getFailedAssumptions();
        std::copy(assumptions.begin(), assumptions.end(), std::back_inserter(_result.solution));
    }
    return _result;
}

int ThreadedSatJob::appl_solved() {

    int result = -1;

    // Already reported the actual result, or still initializing
    if (_done_locally || !_initialized || getState() != ACTIVE) {
        return result;
    }

    auto lock = _solver_lock.getLock();
    result = getSolver()->solveLoop();

    // Did a solver find a result?
    if (result >= 0) {
        _done_locally = true;
        Console::log_send(Console::INFO, getJobTree().getRootNodeRank(), "%s : found result %s", toStr(), 
                            result == RESULT_SAT ? "SAT" : result == RESULT_UNSAT ? "UNSAT" : "UNKNOWN");
        _result_code = result;
    }
    return result;
}

void ThreadedSatJob::appl_dumpStats() {

    if (!_initialized || getState() != ACTIVE) return;
    auto lock = _solver_lock.getLock();

    getSolver()->dumpStats(/*final=*/false);
    if (_time_of_start_solving <= 0) return;
    
    std::vector<long> threadTids = getSolver()->getSolverTids();
    for (size_t i = 0; i < threadTids.size(); i++) {
        if (threadTids[i] < 0) continue;
        double cpuRatio; float sysShare;
        bool ok = Proc::getThreadCpuRatio(threadTids[i], cpuRatio, sysShare);
        if (ok) Console::log(Console::VERB, "%s td.%ld : %.2f%% CPU -> %.2f%% systime", 
                toStr(), threadTids[i], cpuRatio, 100*sysShare);
    }
}

bool ThreadedSatJob::appl_isDestructible() {
    if (!_initialized) return false;
    auto lock = _solver_lock.getLock();
    return _solver->isCleanedUp();
}

bool ThreadedSatJob::appl_wantsToBeginCommunication() {
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

void ThreadedSatJob::appl_beginCommunication() {
    if (!_initialized || getState() != ACTIVE) return;
    Console::log(Console::VVVVERB, "begincomm");
    if (!_solver_lock.tryLock()) return;
    ((AnytimeSatClauseCommunicator*) _clause_comm)->sendClausesToParent();
    if (getJobTree().isLeaf()) _time_of_last_comm = Timer::elapsedSeconds();
    _solver_lock.unlock();
}

void ThreadedSatJob::appl_communicate(int source, JobMessage& msg) {
    if (!_initialized || getState() != ACTIVE) return;
    Console::log(Console::VVVVERB, "comm");
    auto lock = _solver_lock.getLock();
    ((AnytimeSatClauseCommunicator*) _clause_comm)->handle(source, msg);
}

bool ThreadedSatJob::isInitialized() {
    if (!_initialized) return false;
    auto lock = _solver_lock.getLock();
    return _solver->isFullyInitialized();
}
void ThreadedSatJob::prepareSharing(int maxSize) {
    _clause_buffer.resize(maxSize);
    int actualSize = _solver->prepareSharing(_clause_buffer.data(), maxSize);
    _clause_buffer.resize(actualSize);
}
bool ThreadedSatJob::hasPreparedSharing() {
    return !_clause_buffer.empty();
}
std::vector<int> ThreadedSatJob::getPreparedClauses() {
    std::vector<int> out = _clause_buffer;
    _clause_buffer.clear();
    return out;
}
void ThreadedSatJob::digestSharing(const std::vector<int>& clauses) {
    if (!_initialized) return;
    auto lock = _solver_lock.getLock();
    _solver->digestSharing(clauses);
}

ThreadedSatJob::~ThreadedSatJob() {
    Console::log(Console::VVERB, "%s : enter destructor", toStr());
    if (_init_thread.joinable()) _init_thread.join();
    if (_destroy_thread.joinable()) _destroy_thread.join();
    Console::log(Console::VVERB, "%s : destructing SAT job", toStr());
}