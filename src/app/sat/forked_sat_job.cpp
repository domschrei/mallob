
#include <thread>
#include "assert.h"

#include "util/logger.hpp"
#include "util/sys/timer.hpp"
#include "comm/mympi.hpp"
#include "forked_sat_job.hpp"
#include "anytime_sat_clause_communicator.hpp"
#include "horde_shared_memory.hpp"
#include "util/sys/proc.hpp"
#include "util/sys/process.hpp"
#include "horde_config.hpp"

ForkedSatJob::ForkedSatJob(const Parameters& params, int commSize, int worldRank, int jobId) : 
        BaseSatJob(params, commSize, worldRank, jobId), _job_comm_period(params.getFloatParam("s")) {
}

void ForkedSatJob::appl_start() {

    assert(!_initialized);
    assert(!_init_thread.joinable());

    Parameters hParams(_params);
    HordeConfig::applyDefault(hParams, *this);

    const JobDescription& desc = getDescription();
    log(V4_VVER, "%s : rev. %i\n", toStr(), desc.getRevision());

    _solver.reset(new HordeProcessAdapter(hParams,
        desc.getFormulaPayloadSize(0), 
        desc.getFormulaPayload(0), 
        // do not import assumptions if they belong to a later revision
        desc.getRevision() > 0 ? 0 : desc.getAssumptionsSize(),
        desc.getAssumptionsPayload()
    ));
    loadIncrements();
    _clause_comm = (void*) new AnytimeSatClauseCommunicator(hParams, this);

    //log(V5_DEBG, "%s : beginning to solve\n", toStr());
    _solver_pid = _solver->run();

    //log(V4_VVER, "%s : spawned child pid=%i\n", toStr(), _solver_pid);
    _time_of_start_solving = Timer::elapsedSeconds();

    {
        auto lock = _solver_state_change_mutex.getLock();
        auto state = getState();
        if (state == SUSPENDED) 
            _solver->setSolvingState(SolvingStates::SUSPENDED);
        if (state == INACTIVE || state == PAST) 
            _solver->setSolvingState(SolvingStates::STANDBY);
        if (state == PAST) {
            _solver->setSolvingState(SolvingStates::ABORTING);
        }
        _initialized = true;
    }
    
    _init_thread = std::thread([this]() {
        
    });
}

void ForkedSatJob::loadIncrements() {
    const auto& desc = getDescription();
    int lastRev = desc.getRevision();
    std::vector<HordeProcessAdapter::RevisionData> revisions;
    while (_last_imported_revision < lastRev) {
        _last_imported_revision++;
        size_t numLits = desc.getFormulaPayloadSize(_last_imported_revision);
        size_t numAssumptions = _last_imported_revision == lastRev ? desc.getAssumptionsSize() : 0;
        log(V4_VVER, "%s : Load rev. %i : %i lits, %i assumptions\n", toStr(), 
                _last_imported_revision, numLits, numAssumptions);
        revisions.emplace_back(HordeProcessAdapter::RevisionData {
            _last_imported_revision,
            _last_imported_revision == lastRev ? desc.getChecksum() : Checksum(),
            numLits, 
            desc.getFormulaPayload(_last_imported_revision),
            numAssumptions,
            desc.getAssumptionsPayload()
        });
    }
    if (!revisions.empty()) _solver->appendRevisions(revisions);
    _done_locally = false;
    _internal_result = JobResult();
}

void ForkedSatJob::appl_suspend() {
    if (!_initialized) return;
    auto lock = _solver_state_change_mutex.getLock();
    _solver->setSolvingState(SolvingStates::SUSPENDED);
}

void ForkedSatJob::appl_resume() {
    if (!_initialized) return;
    auto lock = _solver_state_change_mutex.getLock();
    _solver->setSolvingState(SolvingStates::ACTIVE);
}

void ForkedSatJob::appl_stop() {
    if (!_initialized) return;
    auto lock = _solver_state_change_mutex.getLock();
    _solver->setSolvingState(SolvingStates::STANDBY);
}

void ForkedSatJob::appl_interrupt() {
    if (!_initialized) return;
    auto lock = _solver_state_change_mutex.getLock();
    _solver->setSolvingState(SolvingStates::STANDBY);
}

void ForkedSatJob::appl_restart() {
    if (!_initialized) return;
    auto lock = _solver_state_change_mutex.getLock();
    loadIncrements();
    _solver->setSolvingState(SolvingStates::ACTIVE);
}

void ForkedSatJob::appl_terminate() {
    if (!_initialized) return;
    auto lock = _solver_state_change_mutex.getLock();
    _solver->setSolvingState(SolvingStates::ABORTING);
    startDestructThreadIfNecessary();
}

int ForkedSatJob::appl_solved() {
    int result = -1;
    if (!_initialized || getState() != ACTIVE) return result;
    if (_done_locally) return result;

    // Did a solver find a result?
    if (_solver->check()) {
        auto solution = _solver->getSolution();
        result = solution.first;
        log(LOG_ADD_DESTRANK | V2_INFO, "%s rev. %i : found result %s", getJobTree().getRootNodeRank(), toStr(), getRevision(), 
                            result == RESULT_SAT ? "SAT" : result == RESULT_UNSAT ? "UNSAT" : "UNKNOWN");
        _internal_result.id = getId();
        _internal_result.result = result;
        _internal_result.revision = getRevision();
        _internal_result.solution = std::move(solution.second);
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
    assert(getState() == PAST);
    // Not initialized (yet): No init thread may be running
    if (!_initialized) return !_init_thread.joinable();
    // If shared memory needs to be cleaned up, start an according thread
    startDestructThreadIfNecessary();
    // Everything cleaned up?
    return _shmem_freed;
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
    bool wants = ((AnytimeSatClauseCommunicator*) _clause_comm)->canSendClauses();
    return wants;
}

void ForkedSatJob::appl_beginCommunication() {
    if (!_initialized || getState() != ACTIVE) return;
    log(V5_DEBG, "begincomm\n");
    ((AnytimeSatClauseCommunicator*) _clause_comm)->sendClausesToParent();
    if (getJobTree().isLeaf()) _time_of_last_comm = Timer::elapsedSeconds();
}

void ForkedSatJob::appl_communicate(int source, JobMessage& msg) {
    if (!_initialized || getState() != ACTIVE) return;
    log(V5_DEBG, "comm\n");
    ((AnytimeSatClauseCommunicator*) _clause_comm)->handle(source, msg);
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
std::vector<int> ForkedSatJob::getPreparedClauses() {
    if (!_initialized) return std::vector<int>();
    return _solver->getCollectedClauses();
}
void ForkedSatJob::digestSharing(std::vector<int>& clauses) {
    if (!_initialized) return;
    _solver->digestClauses(clauses);
    if (getJobTree().isRoot()) {
        log(V2_INFO, "%s : Digested clause buffer of size %ld\n", toStr(), clauses.size());
    }
}

void ForkedSatJob::startDestructThreadIfNecessary() {
    // Ensure concurrent destruction of shared memory
    if (!_destruct_thread.joinable() && !_shmem_freed) {
        log(V4_VVER, "%s : freeing mem\n", toStr());
        _destruct_thread = std::thread([this]() {
            while (!Process::didChildExit(_solver_pid))
                usleep(100*1000); // 0.1s
            _solver->freeSharedMemory();
            log(V4_VVER, "%s : mem freed\n", toStr());
            _shmem_freed = true;
        });
    }
}

ForkedSatJob::~ForkedSatJob() {
    log(V4_VVER, "%s : enter destructor\n", toStr());

    if (_init_thread.joinable()) _init_thread.join();
    if (_destruct_thread.joinable()) _destruct_thread.join();

    if (_initialized) {
        delete (AnytimeSatClauseCommunicator*)_clause_comm;
        _clause_comm = NULL;
        _solver = NULL;
    }

    log(V4_VVER, "%s : destructed SAT job\n", toStr());
}