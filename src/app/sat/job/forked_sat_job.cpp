
#include <thread>
#include "util/assert.hpp"

#include "util/logger.hpp"
#include "util/sys/timer.hpp"
#include "comm/mympi.hpp"
#include "forked_sat_job.hpp"
#include "anytime_sat_clause_communicator.hpp"
#include "util/sys/proc.hpp"
#include "util/sys/process.hpp"
#include "sat_process_config.hpp"
#include "util/sys/thread_pool.hpp"

std::atomic_int ForkedSatJob::_static_subprocess_index = 1;

ForkedSatJob::ForkedSatJob(const Parameters& params, const JobSetup& setup) : 
        BaseSatJob(params, setup) {
}

void ForkedSatJob::appl_start() {
    assert(!_initialized);
    doStartSolver();
    _time_of_start_solving = Timer::elapsedSeconds();
    _initialized = true;
}

void ForkedSatJob::doStartSolver() {

    SatProcessConfig config(_params, *this, _static_subprocess_index++);
    Parameters hParams(_params);
    hParams.satEngineConfig.set(config.toString());
    hParams.applicationConfiguration.set(getDescription().getAppConfiguration().serialize());
    if (_params.verbosity() >= V5_DEBG) LOG(V5_DEBG, "Program options: %s\n", hParams.getParamsAsString().c_str());
    _last_imported_revision = 0;

    const JobDescription& desc = getDescription();
    // do not copy the entire job description if the spawned job is an empty dummy
    bool dummyJob = config.threads == 0; 

    _solver.reset(new SatProcessAdapter(
        std::move(hParams), std::move(config), this,
        dummyJob ? std::min(1ul, desc.getFormulaPayloadSize(0)) : desc.getFormulaPayloadSize(0), 
        desc.getFormulaPayload(0), 
        dummyJob ? std::min(1ul, desc.getAssumptionsSize(0)) : desc.getAssumptionsSize(0),
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
    std::vector<SatProcessAdapter::RevisionData> revisions;
    while (_last_imported_revision < lastRev) {
        _last_imported_revision++;
        size_t numLits = desc.getFormulaPayloadSize(_last_imported_revision);
        size_t numAssumptions = desc.getAssumptionsSize(_last_imported_revision);
        LOG(V4_VVER, "%s : Forward rev. %i : %i lits, %i assumptions\n", toStr(), 
                _last_imported_revision, numLits, numAssumptions);
        revisions.emplace_back(SatProcessAdapter::RevisionData {
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
    if (checkClauseComm()) ((AnytimeSatClauseCommunicator*) _clause_comm)->communicate();
}

void ForkedSatJob::appl_resume() {
    if (!_initialized) return;
    _solver->setSolvingState(SolvingStates::ACTIVE);
    if (checkClauseComm()) ((AnytimeSatClauseCommunicator*) _clause_comm)->communicate();
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

    // Is there still a crash to be handled?
    if (_crash_pending) {
        if (!checkClauseComm() || _solver->getClauseComm()->isDestructible()) {
            // clause comm. can be cleaned up now: handle crash.
            _crash_pending = false;
            handleSolverCrash();
        }
        return result;
    }

    // Did a solver find a result?
    auto status = _solver->check();
    if (status == SatProcessAdapter::FOUND_RESULT) {
        _internal_result = std::move(_solver->getSolution());
        result = _internal_result.result;
        LOG_ADD_DEST(V2_INFO, "%s rev. %i : found result %s", getJobTree().getRootNodeRank(), toStr(), getRevision(), 
                            result == RESULT_SAT ? "SAT" : result == RESULT_UNSAT ? "UNSAT" : "UNKNOWN");
        _internal_result.id = getId();
        _internal_result.revision = getRevision();
        _done_locally = true;
    } else if (status == SatProcessAdapter::CRASHED) {
        // Subprocess crashed for whatever reason: try to recover
        if (checkClauseComm() && !_solver->getClauseComm()->isDestructible()) {
            // Cannot clean up clause comm. right now: remember pending crash handling
            _crash_pending = true;
        } else {
            handleSolverCrash();
        }
    }
    return result;
}

void ForkedSatJob::handleSolverCrash() {

    // Release "old" solver / clause comm from ownership, clean up concurrently
    _solver->releaseClauseComm(); // release clause comm from ownership of the solver
    SatProcessAdapter* solver = _solver.release();
    auto future = ProcessWideThreadPool::get().addTask([solver]() {
        // clean up solver (without the clause comm)
        delete solver;
    });
    _old_solver_destructions.push_back(std::move(future));

    // Start new solver (with renamed shared memory segments)
    doStartSolver();
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

void ForkedSatJob::appl_memoryPanic() {
    if (!_initialized) return;
    int nbThreads = getNumThreads();
    if (nbThreads > 0 && _solver->getStartedNumThreads() == nbThreads) 
        setNumThreads(nbThreads-1);
    LOG(V1_WARN, "[WARN] %s : memory panic triggered - restarting solver with %i threads\n", toStr(), getNumThreads());
    _solver->crash();
}

bool ForkedSatJob::checkClauseComm() {
    if (!_initialized) return false;
    if (_clause_comm == nullptr && _solver->hasClauseComm()) 
        _clause_comm = (void*)_solver->getClauseComm();
    return _clause_comm != nullptr;
}

void ForkedSatJob::appl_communicate() {
    if (!checkClauseComm()) return;
    ((AnytimeSatClauseCommunicator*) _clause_comm)->communicate();
}

void ForkedSatJob::appl_communicate(int source, int mpiTag, JobMessage& msg) {
    if (!checkClauseComm()) {
        if (!msg.returnedToSender) {
            msg.returnedToSender = true;
            MyMpi::isend(source, mpiTag, msg);
        }
        return;
    }
    ((AnytimeSatClauseCommunicator*) _clause_comm)->handle(source, mpiTag, msg);
}

bool ForkedSatJob::isInitialized() {
    return _initialized && _solver->isFullyInitialized();
}

void ForkedSatJob::prepareSharing(int maxSize) {
    if (!_initialized) return;
    _solver->collectClauses(maxSize);
}
bool ForkedSatJob::hasPreparedSharing() {
    if (!_initialized) return true;
    return _solver->hasCollectedClauses();
}
std::vector<int> ForkedSatJob::getPreparedClauses(Checksum& checksum) {
    if (!_initialized || !_solver->hasCollectedClauses()) 
        return std::vector<int>();
    return _solver->getCollectedClauses();
}
std::pair<int, int> ForkedSatJob::getLastAdmittedClauseShare() {
    if (!_initialized) return std::pair<int, int>();
    return _solver->getLastAdmittedClauseShare();
}

void ForkedSatJob::filterSharing(std::vector<int>& clauses) {
    if (!_initialized) return;
    _solver->filterClauses(clauses);
}
bool ForkedSatJob::hasFilteredSharing() {
    if (!_initialized) return false;
    return _solver->hasFilteredClauses();
}
std::vector<int> ForkedSatJob::getLocalFilter() {
    if (!_initialized) return std::vector<int>();
    return _solver->getLocalFilter();
}
void ForkedSatJob::applyFilter(std::vector<int>& filter) {
    if (!_initialized) return;
    _solver->applyFilter(filter);
}

void ForkedSatJob::digestSharingWithoutFilter(std::vector<int>& clauses) {
    if (!_initialized) return;
    _solver->digestClausesWithoutFilter(clauses);
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

    // Wait for destruction of old solvers
    for (auto& future : _old_solver_destructions) {
        if (future.valid()) future.get();
    }

    LOG(V5_DEBG, "%s : destructed FSJ\n", toStr());
}