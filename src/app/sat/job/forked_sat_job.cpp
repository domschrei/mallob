
#include <assert.h>
#include <string.h>
#include <algorithm>
#include <utility>

#include "app/app_message_subscription.hpp"
#include "app/sat/job/formula_shmem_cache.hpp"
#include "util/logger.hpp"
#include "util/sys/timer.hpp"
#include "forked_sat_job.hpp"
#include "anytime_sat_clause_communicator.hpp"
#include "sat_process_config.hpp"
#include "util/sys/thread_pool.hpp"
#include "app/job_tree.hpp"
#include "app/sat/data/clause_metadata.hpp"
#include "app/sat/execution/solving_state.hpp"
#include "app/sat/job/base_sat_job.hpp"
#include "app/sat/job/sat_constants.h"
#include "app/sat/job/sat_process_adapter.hpp"
#include "comm/msgtags.h"
#include "data/app_configuration.hpp"
#include "data/checksum.hpp"
#include "data/job_description.hpp"
#include "data/job_state.h"
#include "data/job_transfer.hpp"
#include "util/option.hpp"
#include "util/params.hpp"

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
    int missingRev;
    bool someMissing = !hasAllDescriptionsForSolving(missingRev);
    while (_last_imported_revision < lastRev) {
        if (someMissing && _last_imported_revision+1 == missingRev) break;
        _last_imported_revision++;
        size_t numLits = desc.getFormulaPayloadSize(_last_imported_revision);
        size_t numAssumptions = desc.getAssumptionsSize(_last_imported_revision);
        LOG(V4_VVER, "%s : Forward rev. %i : %i lits, %i assumptions\n", toStr(), 
                _last_imported_revision, numLits, numAssumptions);
        if (_last_imported_revision < _formulas_in_shmem.size() && _formulas_in_shmem[_last_imported_revision].data) {
            // there is a shared memory segment
            _solver->preregisterShmemObject(std::move(_formulas_in_shmem[_last_imported_revision]));
            _formulas_in_shmem[_last_imported_revision].data = nullptr;
        }
        revisions.emplace_back(SatProcessAdapter::RevisionData {
            _last_imported_revision,
            _last_imported_revision == lastRev ? desc.getChecksum() : Checksum(),
            numLits,
            desc.isRevisionIncomplete(_last_imported_revision) ? nullptr : desc.getFormulaPayload(_last_imported_revision),
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
    loadIncrements();
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
                && _params.proofOutputFile.isSet() && _params.distributedProofAssembly()) {
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

    // Gently ask the solver process to reduce its number of solvers
    LOG(V1_WARN, "[WARN] %s : memory panic triggered - reducing thread count\n", toStr());
    _solver->reduceThreadCount();

    // Straight up crash the solver process, restart with reduced # solvers
    //LOG(V1_WARN, "[WARN] %s : memory panic triggered - restarting solver with %i threads\n", toStr(), getNumThreads());
    //_solver->crash();
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
    if (!isInitialized() || getState() != ACTIVE) return;
    _solver->collectClauses(_clsbuf_export_limit);
}
bool ForkedSatJob::hasPreparedSharing() {
    if (!isInitialized() || getState() != ACTIVE) {
        // wait for prepared clauses in case of deterministic solving
        return !_params.deterministicSolving();
    }
    bool hasCollected = _solver->hasCollectedClauses();
    if (!hasCollected) prepareSharing();
    return hasCollected;
}
std::vector<int> ForkedSatJob::getPreparedClauses(Checksum& checksum, int& successfulSolverId, int& numLits) {
    successfulSolverId = -1;
    numLits = 0;
    return _solver->getCollectedClauses(successfulSolverId, numLits);
}
int ForkedSatJob::getLastAdmittedNumLits() {
    if (!_initialized) return 0;
    return _solver->getLastAdmittedNumLits();
}
void ForkedSatJob::setClauseBufferRevision(int revision) {
    if (!isInitialized()) return;
    _solver->setClauseBufferRevision(revision);
}

void ForkedSatJob::filterSharing(int epoch, std::vector<int>& clauses) {
    if (!isInitialized()) return;
    _solver->filterClauses(epoch, clauses);
}
bool ForkedSatJob::hasFilteredSharing(int epoch) {
    if (!isInitialized() || getState() != ACTIVE) return true;
    return _solver->hasFilteredClauses(epoch);
}
std::vector<int> ForkedSatJob::getLocalFilter(int epoch) {
    if (!isInitialized()) return std::vector<int>(ClauseMetadata::enabled() ? 2 : 0, 0);
    return _solver->getLocalFilter(epoch);
}
void ForkedSatJob::applyFilter(int epoch, std::vector<int>& filter) {
    if (!isInitialized()) return;
    _solver->applyFilter(epoch, filter);
}
void ForkedSatJob::digestSharingWithoutFilter(int epoch, std::vector<int>& clauses, bool stateless) {
    if (!isInitialized()) return;
    _solver->digestClausesWithoutFilter(epoch, clauses, stateless);
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
            // manually clean up formulas in shared memory which haven't been forwarded yet
            for (auto& obj : _formulas_in_shmem) {
                if (!obj.data) continue;
                int descriptionId = getDescription().getJobDescriptionId(obj.revision);
                StaticFormulaSharedMemoryCache::get().drop(descriptionId, obj.userLabel, obj.size, obj.data);
                obj.data = nullptr;
            }
            LOG(V4_VVER, "%s : FSJ mem freed\n", toStr());
            _shmem_freed = true;
        });
    }
}

bool ForkedSatJob::canHandleIncompleteRevision(int rev) {
    if (rev < _formulas_in_shmem.size() && _formulas_in_shmem[rev].data) return true;
    // Probe if the job description of the revision is present
    // as a shared memory segment. This check *must* also add a reference
    // to it (so that it doesn't get deleted in the meantime).
    if (!hasDescription()) return false;
    int descriptionId = getDescription().getJobDescriptionId(rev);
    if (descriptionId == 0) return false;
    size_t size = getDescription().getFormulaPayloadSize(rev);
    std::string shmemId;
    std::string userLabel = "/edu.kit.iti.mallob."
        + std::to_string(getMyMpiRank()) + ".nopidyet"
        + std::string(toStr()) + "~" + std::to_string(_static_subprocess_index)
        + ".formulae." + std::to_string(rev);
    void* shmem = StaticFormulaSharedMemoryCache::get().tryAccess(descriptionId,
        userLabel, size, shmemId);
    if (!shmem) return false;
    if (_formulas_in_shmem.size() <= rev) _formulas_in_shmem.resize(2*rev+1);
    _formulas_in_shmem[rev] = {std::move(shmemId), shmem, size, true, rev, std::move(userLabel)};
    return true;
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