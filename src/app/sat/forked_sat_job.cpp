
#include <thread>
#include "util/assert.hpp"

#include "util/logger.hpp"
#include "util/sys/timer.hpp"
#include "comm/mympi.hpp"
#include "forked_sat_job.hpp"
#include "anytime_sat_clause_communicator.hpp"
#include "horde_shared_memory.hpp"
#include "util/sys/proc.hpp"
#include "util/sys/process.hpp"
#include "horde_config.hpp"
#include "util/sys/thread_pool.hpp"

std::atomic_int ForkedSatJob::_static_subprocess_index = 1;

ForkedSatJob::ForkedSatJob(const Parameters& params, int commSize, int worldRank, int jobId, JobDescription::Application appl) : 
        BaseSatJob(params, commSize, worldRank, jobId, appl), _job_comm_period(params.appCommPeriod()) {
}

void ForkedSatJob::appl_start() {
    assert(!_initialized);
    doStartSolver();
    _time_of_start_solving = Timer::elapsedSeconds();
    _time_of_last_comm = _time_of_start_solving;
    _initialized = true;
}

void ForkedSatJob::doStartSolver() {

    HordeConfig config(_params, *this, _static_subprocess_index++);
    Parameters hParams(_params);
    hParams.hordeConfig.set(config.toString());
    hParams.applicationConfiguration.set(getDescription().getAppConfiguration().serialize());
    if (_params.verbosity() >= V5_DEBG) hParams.printParams();
    _last_imported_revision = 0;

    const JobDescription& desc = getDescription();

    _solver.reset(new HordeProcessAdapter(
        std::move(hParams), std::move(config), this,
        desc.getFormulaPayloadSize(0), 
        desc.getFormulaPayload(0), 
        desc.getAssumptionsSize(0),
        desc.getAssumptionsPayload(0),
        (AnytimeSatClauseCommunicator*)_clause_comm
    ));
    loadIncrements();

    //log(V5_DEBG, "%s : beginning to solve\n", toStr());
    _solver->run();

    if (_initialized) {
        // solver was already initialized before and was then restarted: 
        // Re-learn all historic clauses which the communicator still remembers
        ((AnytimeSatClauseCommunicator*)_clause_comm)->feedHistoryIntoSolver();
    }
}

void ForkedSatJob::loadIncrements() {
    const auto& desc = getDescription();
    int lastRev = desc.getRevision();
    std::vector<HordeProcessAdapter::RevisionData> revisions;
    while (_last_imported_revision < lastRev) {
        _last_imported_revision++;
        size_t numLits = desc.getFormulaPayloadSize(_last_imported_revision);
        size_t numAssumptions = desc.getAssumptionsSize(_last_imported_revision);
        LOG(V4_VVER, "%s : Forward rev. %i : %i lits, %i assumptions\n", toStr(), 
                _last_imported_revision, numLits, numAssumptions);
        revisions.emplace_back(HordeProcessAdapter::RevisionData {
            _last_imported_revision,
            _last_imported_revision == lastRev ? desc.getChecksum() : Checksum(),
            numLits, 
            desc.getFormulaPayload(_last_imported_revision),
            numAssumptions,
            desc.getAssumptionsPayload(_last_imported_revision)
        });
    }
    if (!revisions.empty()) {
        _solver->appendRevisions(revisions, getDesiredRevision());
        _done_locally = false;
        _internal_result = JobResult();
    }
}

void ForkedSatJob::appl_suspend() {
    if (!_initialized) return;
    _solver->setSolvingState(SolvingStates::SUSPENDED);
}

void ForkedSatJob::appl_resume() {
    if (!_initialized) return;
    _time_of_last_comm = Timer::elapsedSeconds()-_job_comm_period;
    _solver->setSolvingState(SolvingStates::ACTIVE);
}

void ForkedSatJob::appl_terminate() {
    if (!_initialized) return;
    _solver->setSolvingState(SolvingStates::ABORTING);
    startDestructThreadIfNecessary();
}

int ForkedSatJob::appl_solved() {
    int result = -1;
    if (!_initialized || getState() != ACTIVE) return result;
    loadIncrements();
    if (_done_locally) return result;

    // Did a solver find a result?
    auto status = _solver->check();
    if (status == HordeProcessAdapter::FOUND_RESULT) {
        _internal_result = std::move(_solver->getSolution());
        result = _internal_result.result;
        LOG_ADD_DEST(V2_INFO, "%s rev. %i : found result %s", getJobTree().getRootNodeRank(), toStr(), getRevision(), 
                            result == RESULT_SAT ? "SAT" : result == RESULT_UNSAT ? "UNSAT" : "UNKNOWN");
        _internal_result.id = getId();
        _internal_result.revision = getRevision();
        _done_locally = true;
    } else if (status == HordeProcessAdapter::CRASHED) {
        // Subprocess crashed for whatever reason: try to recover

        // Release "old" solver / clause comm from ownership, clean up concurrently
        checkClauseComm(); // store clause comm in this job instance (if present)
        _solver->releaseClauseComm(); // release clause comm from ownership of the solver
        HordeProcessAdapter* solver = _solver.release();
        ProcessWideThreadPool::get().addTask([solver]() {
            // clean up solver (without the clause comm)
            delete solver;
        });

        // Start new solver (with renamed shared memory segments)
        doStartSolver();
    }
    return result;
}

JobResult&& ForkedSatJob::appl_getResult() {
    return std::move(_internal_result);
}

void ForkedSatJob::appl_dumpStats() {
    if (!_initialized || getState() != ACTIVE) return;
    _solver->dumpStats();
}

bool ForkedSatJob::appl_isDestructible() {
    assert(getState() == PAST);
    // Not initialized (yet)?
    if (!_initialized) return true;
    // If shared memory needs to be cleaned up, start an according thread
    startDestructThreadIfNecessary();
    // Everything cleaned up?
    return _shmem_freed;
}

bool ForkedSatJob::checkClauseComm() {
    if (!_initialized) return false;
    if (_clause_comm == nullptr && _solver->hasClauseComm()) 
        _clause_comm = (void*)_solver->getClauseComm();
    return _clause_comm != nullptr;
}

bool ForkedSatJob::appl_wantsToBeginCommunication() {
    if (!_initialized || getState() != ACTIVE || _job_comm_period <= 0) return false;
    if (!checkClauseComm()) return false;
    // Special "timed" condition: At least X seconds since last communication 
    if (Timer::elapsedSeconds()-_time_of_last_comm < _job_comm_period) return false;
    // Time has come: Prepare a new buffer of clauses
    bool wants = ((AnytimeSatClauseCommunicator*) _clause_comm)->canSendClauses();
    return wants && (getAgeSinceActivation() >= 0.5 * _job_comm_period);
}

void ForkedSatJob::appl_beginCommunication() {
    if (!_initialized || getState() != ACTIVE) return;
    if (!checkClauseComm()) return;
    LOG(V5_DEBG, "begincomm\n");
    ((AnytimeSatClauseCommunicator*) _clause_comm)->sendClausesToParent();
}

void ForkedSatJob::appl_communicate(int source, JobMessage& msg) {
    if (!_initialized || getState() != ACTIVE) return;
    if (!checkClauseComm()) return;
    LOG(V5_DEBG, "comm\n");
    ((AnytimeSatClauseCommunicator*) _clause_comm)->handle(source, msg);
    if (appl_wantsToBeginCommunication()) appl_beginCommunication();
}

bool ForkedSatJob::isInitialized() {
    return _initialized && _solver->isFullyInitialized();
}

void ForkedSatJob::prepareSharing(int maxSize) {
    if (!_initialized) return;
    _solver->collectClauses(maxSize);
}
bool ForkedSatJob::hasPreparedSharing() {
    if (!_initialized) return false;
    return _solver->hasCollectedClauses();
}
std::vector<int> ForkedSatJob::getPreparedClauses(Checksum& checksum) {
    if (!_initialized) return std::vector<int>();
    return _solver->getCollectedClauses(checksum);
}
void ForkedSatJob::resetLastCommTime() {
    _time_of_last_comm += _job_comm_period;
}

void ForkedSatJob::digestSharing(std::vector<int>& clauses, const Checksum& checksum) {
    if (!_initialized) return;
    _solver->digestClauses(clauses, checksum);
    if (getJobTree().isRoot()) {
        LOG(V3_VERB, "%s : Digested clause buffer of size %ld\n", toStr(), clauses.size());
    }
}
void ForkedSatJob::returnClauses(std::vector<int>& clauses) {
    if (!_initialized) return;
    _solver->returnClauses(clauses);
}

void ForkedSatJob::startDestructThreadIfNecessary() {
    // Ensure concurrent destruction of shared memory
    if (!_destruction.valid() && !_shmem_freed) {
        LOG(V4_VVER, "%s : FSJ freeing mem\n", toStr());
        _destruction = ProcessWideThreadPool::get().addTask([this]() {
            _solver->waitUntilChildExited();
            _solver->freeSharedMemory();
            LOG(V4_VVER, "%s : FSJ mem freed\n", toStr());
            _shmem_freed = true;
        });
    }
}

ForkedSatJob::~ForkedSatJob() {
    LOG(V5_DEBG, "%s : enter FSJ destructor\n", toStr());

    if (_initialized) _solver->setSolvingState(SolvingStates::ABORTING);
    if (_destruction.valid()) _destruction.get();
    if (_initialized) _solver = NULL;

    LOG(V5_DEBG, "%s : destructed FSJ\n", toStr());
}