
#include <thread>
#include "app/app_message_subscription.hpp"
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

ForkedSatJob::ForkedSatJob(const Parameters& params, const JobSetup& setup, AppMessageTable& table) : 
        BaseSatJob(params, setup, table) {
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

    if (!_initialized) {
        _clause_comm.reset(new AnytimeSatClauseCommunicator(_params, this));
    }

    _solver.reset(new SatProcessAdapter(
        std::move(hParams), std::move(config), this,
        dummyJob ? std::min(1ul, desc.getFormulaPayloadSize(0)) : desc.getFormulaPayloadSize(0), 
        desc.getFormulaPayload(0), 
        dummyJob ? std::min(1ul, desc.getAssumptionsSize(0)) : desc.getAssumptionsSize(0),
        desc.getAssumptionsPayload(0),
        _clause_comm
    ));
    loadIncrements();

    //log(V5_DEBG, "%s : beginning to solve\n", toStr());
    _solver->run();
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
    _clause_comm->communicate();
}

void ForkedSatJob::appl_resume() {
    if (!_initialized) return;
    _solver->setSolvingState(SolvingStates::ACTIVE);
    _clause_comm->communicate();
}

void ForkedSatJob::appl_terminate() {
    if (!_initialized) return;
    _solver->setSolvingState(SolvingStates::ABORTING);
}

int ForkedSatJob::appl_solved() {
    int result = -1;
    if (!_initialized || getState() != ACTIVE) return result;
    loadIncrements();
    if (_done_locally || _assembling_proof) {
        if (_assembling_proof && _clause_comm->isDoneAssemblingProof()) {
            _assembling_proof = false;
            return _internal_result.result;
        }
        return result;
    }

    // Did a solver find a result?
    auto status = _solver->check();
    if (status == SatProcessAdapter::FOUND_RESULT) {
        _internal_result = std::move(_solver->getSolution());
        assert(_internal_result.hasSerialization());
        result = _internal_result.result;
        LOG_ADD_DEST(V2_INFO, "%s rev. %i : found result %s", getJobTree().getRootNodeRank(), toStr(), getRevision(), 
                            result == RESULT_SAT ? "SAT" : result == RESULT_UNSAT ? "UNSAT" : "UNKNOWN");
        _internal_result.id = getId();
        _internal_result.revision = getRevision();
        _done_locally = true;

        if (ClauseMetadata::enabled() && result == RESULT_UNSAT
                && _params.distributedProofAssembly()) {
            // Unsatisfiability: handle separately.
            int finalEpoch = _clause_comm->getCurrentEpoch();
            int winningInstance = _internal_result.winningInstanceId;
            unsigned long globalStartOfSuccessEpoch = _internal_result.globalStartOfSuccessEpoch;
            LOG(V2_INFO, "Query to begin distributed proof assembly with winning instance %i, gsofe=%lu\n", 
                winningInstance, globalStartOfSuccessEpoch);
            JobMessage msg(getId(), getJobTree().getRootContextId(), 
                getRevision(), finalEpoch, MSG_NOTIFY_UNSAT_FOUND);
            msg.payload.push_back(winningInstance);
            int size = msg.payload.size();
            msg.payload.resize(msg.payload.size()+2);
            memcpy(msg.payload.data()+size, &globalStartOfSuccessEpoch, 2*sizeof(int));
            getJobTree().sendToRoot(msg);
            _assembling_proof = true;
            return -1;
        }

    } else if (status == SatProcessAdapter::CRASHED) {
        // Subprocess crashed for whatever reason: try to recover
        handleSolverCrash();
    }
    return result;
}

void ForkedSatJob::handleSolverCrash() {

    // Release "old" solver from ownership, clean up concurrently
    SatProcessAdapter* solver = _solver.release();
    auto future = ProcessWideThreadPool::get().addTask([solver]() {
        // clean up solver
        delete solver;
    });
    _old_solver_destructions.push_back(std::move(future));

    // Start new solver (with renamed shared memory segments)
    doStartSolver();
}

JobResult&& ForkedSatJob::appl_getResult() {
    assert(_internal_result.hasSerialization());
    return std::move(_internal_result);
}

void ForkedSatJob::appl_dumpStats() {
    if (!_initialized || getState() != ACTIVE) return;
    _solver->dumpStats();
}

bool ForkedSatJob::appl_isDestructible() {
    // Wrong state?
    if (getState() != PAST) return false;
    // Not initialized (yet)?
    if (!_initialized) return true;
    // SAT comm. present which is not destructible (yet)?
    if (!_clause_comm->isDestructible()) {
        _clause_comm->communicate(); // may advance destructibility
        return false;
    }
    // Destructible!
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

void ForkedSatJob::appl_communicate() {
    if (!_clause_comm) return;
    _clause_comm->communicate();
    while (hasDeferredMessage()) {
        auto deferredMsg = getDeferredMessage();
        _clause_comm->handle(
            deferredMsg.source, deferredMsg.mpiTag, deferredMsg.msg);
    }
}

void ForkedSatJob::appl_communicate(int source, int mpiTag, JobMessage& msg) {
    if (!_initialized && (ClauseMetadata::enabled() || _params.deterministicSolving()) 
            && msg.tag == MSG_INITIATE_CLAUSE_SHARING) {
        LOG(V2_INFO, "DEFER MSG <= [%i]\n", source);
        deferMessage(source, mpiTag, msg);
        return;
    }
    if (!_initialized) {
        msg.returnToSender(source, mpiTag);
        return;
    }
    if (msg.tag == MSG_INITIATE_PROOF_COMBINATION) {
        // shut down solver
        if (_solver) _solver->setSolvingState(SolvingStates::ABORTING);
    }
    _clause_comm->handle(source, mpiTag, msg);
}

bool ForkedSatJob::isInitialized() {
    return _initialized && _solver->isFullyInitialized();
}

void ForkedSatJob::prepareSharing() {
    if (!_initialized || getState() != ACTIVE) return;
    _solver->collectClauses(_clsbuf_export_limit);
}
bool ForkedSatJob::hasPreparedSharing() {
    if (!isInitialized() && _params.deterministicSolving()) return false;
    if (!_initialized || getState() != ACTIVE) return !_params.deterministicSolving();
    bool hasCollected = _solver->hasCollectedClauses();
    if (!hasCollected) prepareSharing();
    return hasCollected;
}
std::vector<int> ForkedSatJob::getPreparedClauses(Checksum& checksum, int& successfulSolverId) {
    successfulSolverId = -1;
    if (!_initialized) return std::vector<int>();
    return _solver->getCollectedClauses(successfulSolverId);
}
std::pair<int, int> ForkedSatJob::getLastAdmittedClauseShare() {
    if (!_initialized) return std::pair<int, int>();
    return _solver->getLastAdmittedClauseShare();
}

void ForkedSatJob::filterSharing(int epoch, std::vector<int>& clauses) {
    if (!_initialized) return;
    _solver->filterClauses(epoch, clauses);
}
bool ForkedSatJob::hasFilteredSharing(int epoch) {
    if (!_initialized || getState() != ACTIVE) return true;
    return _solver->hasFilteredClauses(epoch);
}
std::vector<int> ForkedSatJob::getLocalFilter(int epoch) {
    if (!_initialized) return std::vector<int>(ClauseMetadata::numBytes(), 0);
    return _solver->getLocalFilter(epoch);
}
void ForkedSatJob::applyFilter(int epoch, std::vector<int>& filter) {
    if (!_initialized) return;
    _solver->applyFilter(epoch, filter);
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

void ForkedSatJob::digestHistoricClauses(int epochBegin, int epochEnd, std::vector<int>& clauses) {
    if (!_initialized) return;
    _solver->digestHistoricClauses(epochBegin, epochEnd, clauses);
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