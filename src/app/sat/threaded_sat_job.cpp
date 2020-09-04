
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

void ThreadedSatJob::lockHordeManipulation() {
    _horde_manipulation_lock.lock();
}
void ThreadedSatJob::unlockHordeManipulation() {
    _horde_manipulation_lock.unlock();
}

bool ThreadedSatJob::appl_initialize() {

    assert(hasJobDescription());
    //_horde_manipulation_lock.updateName(std::string("HordeManip") + toStr());

    // Initialize Hordesat instance
    assert(_solver == NULL || Console::fail("Solver is not NULL! State of %s : %s", toStr(), jobStateToStr()));

    auto lock = _horde_manipulation_lock.getLock();

    if (_abort_after_initialization) {
        return false;
    }

    Parameters hParams(_params);
    HordeConfig::applyDefault(hParams, *this);

    Console::log(Console::VERB, "%s : creating horde instance", toStr());
    _solver = std::unique_ptr<HordeLib>(new HordeLib(hParams, std::shared_ptr<LoggingInterface>(new ConsoleHordeInterface(
        "<h-" + std::string(toStr()) + ">", "#" + std::to_string(getId()) + "."
    ))));
    _clause_comm = (void*) new AnytimeSatClauseCommunicator(hParams, this);

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

bool ThreadedSatJob::appl_doneInitializing() {
    return _solver != NULL && getSolver()->isFullyInitialized();
}

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

void ThreadedSatJob::appl_pause() {
    if (!solverNotNull()) return;
    auto lock = _horde_manipulation_lock.getLock();
    if (solverNotNull()) getSolver()->setPaused();
}

void ThreadedSatJob::appl_unpause() {
    if (!solverNotNull()) return;
    auto lock = _horde_manipulation_lock.getLock();
    if (solverNotNull()) getSolver()->unsetPaused();
}

void ThreadedSatJob::appl_interrupt() {
    if (!solverNotNull()) return;
    auto lock = _horde_manipulation_lock.getLock();
    appl_interrupt_unsafe();
}

void ThreadedSatJob::appl_interrupt_unsafe() {
    if (solverNotNull()) {
        _solver->interrupt(); // interrupt SAT solving (but keeps solver threads!)
        _solver->finishSolving(); // concludes solving process
    }
}

void ThreadedSatJob::cleanUpThread() {
    Console::log(Console::VVERB, "%s : cleanup thread start", toStr());
    auto lock = _horde_manipulation_lock.getLock();
    cleanUp();
    Console::log(Console::VVERB, "%s : cleanup thread done", toStr());
}

void ThreadedSatJob::cleanUp() {
    if (solverNotNull()) _solver->cleanUp();
}

void ThreadedSatJob::appl_withdraw() {

    auto lock = _horde_manipulation_lock.getLock();
    if (isInitializingUnsafe()) {
        _abort_after_initialization = true;
    }
    if (_clause_comm != NULL) {
        delete (AnytimeSatClauseCommunicator*)_clause_comm;
        _clause_comm = NULL;
    }
    if (solverNotNull()) {
        getSolver()->abort();
        // Do cleanup of HordeLib and its threads in a separate thread to avoid blocking
        _bg_thread = std::thread(&ThreadedSatJob::cleanUpThread, this);
    }
}

void ThreadedSatJob::extractResult(int resultCode) {
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

int ThreadedSatJob::appl_solveLoop() {

    int result = -1;

    // Already reported the actual result, or still initializing
    if (_done_locally) {
        return result;
    }

    if (getState() == ACTIVE) result = getSolver()->solveLoop();
    else return result;

    // Did a solver find a result?
    if (result >= 0) {
        _done_locally = true;
        Console::log_send(Console::INFO, getRootNodeRank(), "%s : found result %s", toStr(), 
                            result == 10 ? "SAT" : result == 20 ? "UNSAT" : "UNKNOWN");
        extractResult(result);
    }
    return result;
}

void ThreadedSatJob::appl_dumpStats() {
    if (isInState({ACTIVE})) {

        getSolver()->dumpStats();
        if (_time_of_start_solving <= 0) return;
        
        std::vector<long> threadTids = getSolver()->getSolverTids();
        for (size_t i = 0; i < threadTids.size(); i++) {
            if (threadTids[i] < 0) continue;
            double cpuRatio; float sysShare;
            bool ok = Proc::getThreadCpuRatio(threadTids[i], cpuRatio, sysShare);
            if (ok)
                Console::log(Console::VERB, "%s td.%ld : %.2f%% CPU -> %.2f%% systime", 
                    toStr(), threadTids[i], cpuRatio, 100*sysShare);
        }
    }
}

bool ThreadedSatJob::appl_isDestructible() {
    return !solverNotNull() || _solver->isCleanedUp();
}

bool ThreadedSatJob::appl_wantsToBeginCommunication() const {
    if (_job_comm_period <= 0) return false;
    if (_clause_comm == NULL) return false;
    // Special "timed" conditions for leaf nodes:
    if (isLeaf()) {
        // At least half a second since initialization / reactivation
        if (getAgeSinceActivation() < 0.5 * _job_comm_period) return false;
        // At least params["s"] seconds since last communication 
        if (Timer::elapsedSeconds()-_time_of_last_comm < _job_comm_period) return false;
    }
    bool locked = _horde_manipulation_lock.tryLock();
    if (!locked) return false;
    bool wants = ((AnytimeSatClauseCommunicator*) _clause_comm)->canSendClauses();
    _horde_manipulation_lock.unlock();
    return wants;
}

void ThreadedSatJob::appl_beginCommunication() {
    Console::log(Console::VVVVERB, "begincomm");
    if (_clause_comm == NULL) return;
    auto lock = _horde_manipulation_lock.getLock();
    if (_clause_comm != NULL) 
        ((AnytimeSatClauseCommunicator*) _clause_comm)->sendClausesToParent();
    if (isLeaf()) _time_of_last_comm = Timer::elapsedSeconds();
}

void ThreadedSatJob::appl_communicate(int source, JobMessage& msg) {
    Console::log(Console::VVVVERB, "comm");
    if (_clause_comm == NULL) return;
    auto lock = _horde_manipulation_lock.getLock();
    if (_clause_comm != NULL)
        ((AnytimeSatClauseCommunicator*) _clause_comm)->handle(source, msg);
}

bool ThreadedSatJob::isInitialized() {
    if (!solverNotNull()) return false;
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
    _solver->digestSharing(clauses);
}

ThreadedSatJob::~ThreadedSatJob() {

    Console::log(Console::VVERB, "%s : enter destructor", toStr());
    auto lock = _horde_manipulation_lock.getLock();

    if (_clause_comm != NULL) {
        delete (AnytimeSatClauseCommunicator*)_clause_comm;
        _clause_comm = NULL;
    }

    if (_bg_thread.joinable()) {
        Console::log(Console::VVERB, "%s : joining bg thread", toStr());
        lock.unlock();
        _bg_thread.join(); // if already aborting
        lock.lock();
    }

    if (solverNotNull() && !_solver->isCleanedUp()) {
        Console::log(Console::VVERB, "%s : destruct hordesat", toStr());
        appl_interrupt_unsafe();
        _solver->abort();
        cleanUp();
    }

    Console::log(Console::VVERB, "%s : destructed SAT job", toStr());
}