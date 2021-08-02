
#include <thread>
#include <assert.h>

#include "threaded_sat_job.hpp"

#include "util/logger.hpp"
#include "util/sys/timer.hpp"
#include "anytime_sat_clause_communicator.hpp"
#include "util/sys/proc.hpp"
#include "hordesat/horde.hpp"
#include "horde_config.hpp"

ThreadedSatJob::ThreadedSatJob(const Parameters& params, int commSize, int worldRank, int jobId) : 
        BaseSatJob(params, commSize, worldRank, jobId), _done_locally(false), _job_comm_period(params.appCommPeriod()) {
}

void ThreadedSatJob::appl_start() {

    assert(!_initialized);
    
    // Initialize Hordesat instance
    Parameters hParams(_params);
    HordeConfig config(_params, *this);
    _solver = std::unique_ptr<HordeLib>(new HordeLib(hParams, config, 
        Logger::getMainInstance().copy(
            "<h-" + std::string(toStr()) + ">", "#" + std::to_string(getId()) + "."
        )
    ));
    _clause_comm = (void*) new AnytimeSatClauseCommunicator(hParams, this);

    //log(V5_DEBG, "%s : beginning to solve\n", toStr());
    const JobDescription& desc = getDescription();
    while (_last_imported_revision < desc.getRevision()) {
        _last_imported_revision++;
        _solver->appendRevision(_last_imported_revision, 
            desc.getFormulaPayloadSize(_last_imported_revision), 
            desc.getFormulaPayload(_last_imported_revision), 
            desc.getAssumptionsSize(_last_imported_revision), 
            desc.getAssumptionsPayload(_last_imported_revision)
        );
    }
    _solver->solve();
    //log(V4_VVER, "%s : finished horde initialization\n", toStr());
    _time_of_start_solving = Timer::elapsedSeconds();
    _initialized = true;
}

void ThreadedSatJob::appl_suspend() {
    if (!_initialized) return;
    auto lock = _solver_lock.getLock();
    getSolver()->setPaused();
    ((AnytimeSatClauseCommunicator*)_clause_comm)->suspend();
}

void ThreadedSatJob::appl_resume() {
    if (!_initialized) return;
    auto lock = _solver_lock.getLock();
    getSolver()->unsetPaused();
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
    if (_result.id != 0) return _result;
    auto lock = _solver_lock.getLock();
    _result = getSolver()->getResult();
    _result.id = getId();
    assert(_result.revision == getRevision());
    return _result;
}

int ThreadedSatJob::appl_solved() {

    int result = -1;

    // Import new revisions as necessary
    {
        auto lock = _solver_lock.getLock();
        const JobDescription& desc = getDescription();
        while (_last_imported_revision < desc.getRevision()) {
            _last_imported_revision++;
            _solver->appendRevision(
                _last_imported_revision,
                desc.getFormulaPayloadSize(_last_imported_revision), 
                desc.getFormulaPayload(_last_imported_revision),
                desc.getAssumptionsSize(_last_imported_revision),
                desc.getAssumptionsPayload(_last_imported_revision)
            );
            _done_locally = false;
            _result = JobResult();
            _result_code = 0;
        }
    }

    // Already reported the actual result, or still initializing
    if (_done_locally || !_initialized || getState() != ACTIVE) {
        return result;
    }

    auto lock = _solver_lock.getLock();
    result = getSolver()->solveLoop();

    // Did a solver find a result?
    if (result >= 0) {
        _done_locally = true;
        log(LOG_ADD_DESTRANK | V2_INFO, "%s : found result %s", getJobTree().getRootNodeRank(), toStr(), 
                            result == RESULT_SAT ? "SAT" : result == RESULT_UNSAT ? "UNSAT" : "UNKNOWN");
        _result_code = result;

        // Extract result to avoid later deadlocks
        lock.unlock();
        appl_getResult(); // locks internally
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
        if (ok) log(V3_VERB, "%s td.%ld cpuratio=%.3f sys=%.3f\n", 
                toStr(), threadTids[i], cpuRatio, 100*sysShare);
    }
}

bool ThreadedSatJob::appl_isDestructible() {
    return !_initialized || _solver->isCleanedUp();
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
    log(V5_DEBG, "begincomm\n");
    if (!_solver_lock.tryLock()) return;
    ((AnytimeSatClauseCommunicator*) _clause_comm)->sendClausesToParent();
    if (getJobTree().isLeaf()) _time_of_last_comm = Timer::elapsedSeconds();
    _solver_lock.unlock();
}

void ThreadedSatJob::appl_communicate(int source, JobMessage& msg) {
    if (!_initialized || getState() != ACTIVE) return;
    log(V5_DEBG, "comm\n");
    auto lock = _solver_lock.getLock();
    ((AnytimeSatClauseCommunicator*) _clause_comm)->handle(source, msg);
}

bool ThreadedSatJob::isInitialized() {
    if (!_initialized) return false;
    return _solver->isFullyInitialized();
}
void ThreadedSatJob::prepareSharing(int maxSize) {
    _clause_buffer.resize(maxSize);
    _clause_checksum = Checksum();
    int actualSize = _solver->prepareSharing(_clause_buffer.data(), maxSize, _clause_checksum);
    _clause_buffer.resize(actualSize);
}
bool ThreadedSatJob::hasPreparedSharing() {
    return !_clause_buffer.empty();
}
std::vector<int> ThreadedSatJob::getPreparedClauses(Checksum& checksum) {
    std::vector<int> out = _clause_buffer;
    _clause_buffer.clear();
    checksum = _clause_checksum;
    return out;
}
void ThreadedSatJob::digestSharing(std::vector<int>& clauses, const Checksum& checksum) {
    _solver->digestSharing(clauses, checksum);
    if (getJobTree().isRoot()) {
        log(V2_INFO, "%s : Digested clause buffer of size %ld\n", toStr(), clauses.size());
    }
}

ThreadedSatJob::~ThreadedSatJob() {
    log(V4_VVER, "%s : enter destructor\n", toStr());
    if (_destroy_thread.joinable()) _destroy_thread.join();
    log(V4_VVER, "%s : destructing SAT job\n", toStr());
}