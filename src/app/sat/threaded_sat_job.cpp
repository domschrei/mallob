
#include <thread>
#include "util/assert.hpp"

#include "threaded_sat_job.hpp"

#include "util/logger.hpp"
#include "util/sys/timer.hpp"
#include "anytime_sat_clause_communicator.hpp"
#include "util/sys/proc.hpp"
#include "hordesat/horde.hpp"
#include "horde_config.hpp"
#include "util/sys/thread_pool.hpp"

ThreadedSatJob::ThreadedSatJob(const Parameters& params, int commSize, int worldRank, int jobId,
    JobDescription::Application appl) : 
        BaseSatJob(params, commSize, worldRank, jobId, appl), _done_locally(false), 
        _job_comm_period(params.appCommPeriod()) {}

void ThreadedSatJob::appl_start() {

    assert(!_initialized);
    
    // Initialize Hordesat instance
    Parameters hParams(_params);
    hParams.applicationConfiguration.set(getDescription().getAppConfiguration().serialize());
    HordeConfig config(_params, *this, /*recoveryIndex=*/0);
    _solver = std::unique_ptr<HordeLib>(
        new HordeLib(hParams, config, Logger::getMainInstance())
    );
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
    _time_of_last_comm = _time_of_start_solving;
    _initialized = true;
}

void ThreadedSatJob::appl_suspend() {
    if (!_initialized) return;
    getSolver()->setPaused();
    ((AnytimeSatClauseCommunicator*)_clause_comm)->suspend();
}

void ThreadedSatJob::appl_resume() {
    if (!_initialized) return;
    getSolver()->unsetPaused();
}

void ThreadedSatJob::appl_terminate() {
    if (!_initialized) return;
    if (_destroy_future.valid()) return; 
    _destroy_future = ProcessWideThreadPool::get().addTask([this]() {
        delete (AnytimeSatClauseCommunicator*)_clause_comm;
        _clause_comm = NULL;
        _solver->terminateSolvers();
        _solver->cleanUp();
    });
}

JobResult&& ThreadedSatJob::appl_getResult() {
    if (_result.id != 0) return std::move(_result);
    _result = getSolver()->getResult();
    _result.id = getId();
    assert(_result.revision == getRevision());
    return std::move(_result);
}

int ThreadedSatJob::appl_solved() {

    int result = -1;

    // Import new revisions as necessary
    {
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

    result = getSolver()->solveLoop();

    // Did a solver find a result?
    if (result >= 0) {
        _done_locally = true;
        LOG_ADD_DEST(V2_INFO, "%s : found result %s", getJobTree().getRootNodeRank(), toStr(), 
                            result == RESULT_SAT ? "SAT" : result == RESULT_UNSAT ? "UNSAT" : "UNKNOWN");
        _result_code = result;

        // Extract result
        appl_getResult();
    }
    return result;
}

void ThreadedSatJob::appl_dumpStats() {

    if (!_initialized || getState() != ACTIVE) return;

    getSolver()->dumpStats(/*final=*/false);
    if (_time_of_start_solving <= 0) return;
    
    std::vector<long> threadTids = getSolver()->getSolverTids();
    for (size_t i = 0; i < threadTids.size(); i++) {
        if (threadTids[i] < 0) continue;
        double cpuRatio; float sysShare;
        bool ok = Proc::getThreadCpuRatio(threadTids[i], cpuRatio, sysShare);
        if (ok) LOG(V3_VERB, "%s td.%ld cpuratio=%.3f sys=%.3f\n", 
                toStr(), threadTids[i], cpuRatio, 100*sysShare);
    }
}

bool ThreadedSatJob::appl_isDestructible() {
    return !_initialized || _solver->isCleanedUp();
}

bool ThreadedSatJob::appl_wantsToBeginCommunication() {
    if (!_initialized || getState() != ACTIVE || _job_comm_period <= 0) return false;
    // At least X seconds since last communication 
    if (Timer::elapsedSeconds()-_time_of_last_comm < _job_comm_period) return false;
    bool wants = ((AnytimeSatClauseCommunicator*) _clause_comm)->canSendClauses();
    return wants && (getAgeSinceActivation() < 0.5 * _job_comm_period);
}

void ThreadedSatJob::appl_beginCommunication() {
    if (!_initialized || getState() != ACTIVE) return;
    LOG(V5_DEBG, "begincomm\n");
    ((AnytimeSatClauseCommunicator*) _clause_comm)->sendClausesToParent();
}

void ThreadedSatJob::appl_communicate(int source, JobMessage& msg) {
    if (!_initialized || getState() != ACTIVE) return;
    LOG(V5_DEBG, "comm\n");
    ((AnytimeSatClauseCommunicator*) _clause_comm)->handle(source, msg);
    if (appl_wantsToBeginCommunication()) appl_beginCommunication();
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
void ThreadedSatJob::resetLastCommTime() {
    _time_of_last_comm += _job_comm_period;
}

void ThreadedSatJob::digestSharing(std::vector<int>& clauses, const Checksum& checksum) {
    _solver->digestSharing(clauses, checksum);
    if (getJobTree().isRoot()) {
        LOG(V3_VERB, "%s : Digested clause buffer of size %ld\n", toStr(), clauses.size());
    }
}

void ThreadedSatJob::returnClauses(std::vector<int>& clauses) {
    _solver->returnClauses(clauses.data(), clauses.size());
}

ThreadedSatJob::~ThreadedSatJob() {
    if (!_initialized) return;
    LOG(V5_DEBG, "%s : enter TSJ destructor\n", toStr());
    if (!_destroy_future.valid()) appl_terminate();
    _destroy_future.get();
    LOG(V5_DEBG, "%s : destructed TSJ\n", toStr());
}