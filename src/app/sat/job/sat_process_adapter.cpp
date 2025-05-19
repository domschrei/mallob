
#include <stdlib.h>
#include <assert.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <atomic>
#include <functional>
#include <utility>

#include "app/sat/data/clause_metadata.hpp"
#include "app/sat/execution/clause_pipe_defs.hpp"
#include "app/sat/execution/solving_state.hpp"
#include "app/sat/job/forked_sat_job.hpp"
#include "util/sys/shmem_cache.hpp"
#include "app/sat/job/inplace_sharing_aggregation.hpp"
#include "sat_process_adapter.hpp"
#include "../execution/engine.hpp"
#include "util/string_utils.hpp"
#include "util/sys/bidirectional_anytime_pipe_shmem.hpp"
#include "util/sys/shared_memory.hpp"
#include "util/sys/proc.hpp"
#include "util/sys/subprocess.hpp"
#include "util/sys/process.hpp"
#include "util/logger.hpp"
#include "util/sys/thread_pool.hpp"
#include "app/sat/job/sat_shared_memory.hpp"
#include "util/option.hpp"
#include "util/sys/tmpdir.hpp"
#include "util/sys/watchdog.hpp"

#ifndef MALLOB_SUBPROC_DISPATCH_PATH
#define MALLOB_SUBPROC_DISPATCH_PATH ""
#endif

SatProcessAdapter::SatProcessAdapter(Parameters&& params, SatProcessConfig&& config,
    size_t fSize, const int* fLits, size_t aSize, const int* aLits, Checksum chksum,
    int descId, std::shared_ptr<AnytimeSatClauseCommunicator>& comm) :    
        _params(std::move(params)), _config(std::move(config)), _clause_comm(comm),
        _f_size(fSize), _f_lits(fLits), _a_size(aSize), _a_lits(aLits), _desc_id(descId), _chksum(chksum) {

    _desired_revision = _config.firstrev;
    _shmem_id = _config.getSharedMemId(Proc::getPid());
    assert(_clause_comm);

    // Initialize "management" shared memory
    //log(V4_VVER, "Setup base shmem: %s\n", _shmem_id.c_str());
    void* mainShmem = SharedMemory::create(_shmem_id, sizeof(SatSharedMemory));
    _shmem.insert(ShmemObject{_shmem_id, mainShmem, sizeof(SatSharedMemory)});
    // "placement new" operator: construct object not in the heap but in the provided chunk of memory
    _hsm = new ((char*)mainShmem) SatSharedMemory();
    _hsm->fSize = _f_size;
    _hsm->aSize = _a_size;
    _hsm->chksum = _chksum;
    _hsm->config = _config;
    _sum_of_revision_sizes += _f_size;
}

void SatProcessAdapter::doWriteRevisions() {

    if (_num_revisions_to_write.load(std::memory_order_relaxed) == 0) return;
    if (!_initialized || _hsm->doTerminate || !_mtx_revisions.tryLock()) return;

    if (_bg_writer_running) {
        _mtx_revisions.unlock();
        return;
    }
    if (_bg_writer.valid()) _bg_writer.get();
    
    _bg_writer = ProcessWideThreadPool::get().addTask([this]() {
        while (!_terminate && _num_revisions_to_write > 0) {
            RevisionData revData;
            {
                auto lock = _mtx_revisions.getLock();
                if (_revisions_to_write.empty()) break;
                revData = _revisions_to_write.front();
                _revisions_to_write.erase(_revisions_to_write.begin());
                _num_revisions_to_write--;
            }
            LOG(V4_VVER, "DBG Writing next revision\n");
            auto revStr = std::to_string(revData.revision);
            createSharedMemoryBlock("fsize."       + revStr, sizeof(size_t),              (void*)&revData.fSize);
            createSharedMemoryBlock("asize."       + revStr, sizeof(size_t),              (void*)&revData.aSize);
            const int* fPtr = (const int*) createSharedMemoryBlock("formulae." + revStr, sizeof(int) * revData.fSize,
                (void*)revData.fLits, revData.revision, revData.descriptionId, true);
            LOG(V2_INFO, "SUMMARY %s\n", StringUtils::getSummary(fPtr, revData.fSize).c_str());
            if (revData.fSize > 0) assert(fPtr[0] != 0);
            if (revData.fSize > 0) assert(fPtr[revData.fSize-1] == 0);
            createSharedMemoryBlock("assumptions." + revStr, sizeof(int) * revData.aSize, (void*)revData.aLits);
            createSharedMemoryBlock("checksum."    + revStr, sizeof(Checksum),            (void*)&(revData.checksum));
            _written_revision = revData.revision;
            LOG(V4_VVER, "DBG Done writing next revision %i\n", revData.revision);
        }
        auto lock = _mtx_revisions.getLock();
        _bg_writer_running = false;
    });

    _bg_writer_running = true;
    _mtx_revisions.unlock();
}

void SatProcessAdapter::run() {
    _running = true;
    _bg_initializer = ProcessWideThreadPool::get().addTask(
        std::bind(&SatProcessAdapter::doInitialize, this)
    );
}

void SatProcessAdapter::doInitialize() {

    // Allocate shared memory for formula, assumptions of initial revision
    const int* fInShmem = (const int*) createSharedMemoryBlock("formulae.0",
        sizeof(int) * _f_size, (void*)_f_lits, 0, _desc_id, true);
    LOG(V2_INFO, "SUMMARY %s\n", StringUtils::getSummary(fInShmem, _f_size).c_str());
    if (_f_size > 0) assert(fInShmem[0] != 0);
    if (_f_size > 0) assert(fInShmem[_f_size-1] == 0);
    createSharedMemoryBlock("assumptions.0", sizeof(int) * _a_size, (void*)_a_lits);

    // Set up bi-directional pipe to and from the subprocess
    char* pipeParentToChild = (char*) createSharedMemoryBlock("pipe-parenttochild", _hsm->pipeBufSize, nullptr);
    char* pipeChildToParent = (char*) createSharedMemoryBlock("pipe-childtoparent", _hsm->pipeBufSize, nullptr);
    _guard_pipe.lock()->reset(new BiDirectionalAnytimePipeShmem(
        {pipeParentToChild, _hsm->pipeBufSize, true},
        {pipeChildToParent, _hsm->pipeBufSize, true}, true));

    // Create SAT solving child process
    Subprocess subproc(_params, "mallob_sat_process");
    pid_t res = subproc.start();

    // Set up a watchdog
    auto thisTid = Proc::getTid();
    Watchdog watchdog(_params.watchdog(), 500, true, [&, childPid=res]() {
        // In case of a freeze, trace the child process itself
        Process::writeTrace(childPid);
    });
    watchdog.setWarningPeriod(500);
    watchdog.setAbortPeriod(10'000);

    // Wait until the process is properly initialized
    while (!_hsm->didStart && !Process::didChildExit(res)) {
        Process::resume(res);
        usleep(1000 * 10); // 10 ms
    }

    // Change adapter state
    auto lock = _mtx_state.getLock();
    _child_pid = res;
    _state = SolvingStates::ACTIVE;
    applySolvingState(true);
}

bool SatProcessAdapter::isFullyInitialized() {
    return _initialized && _hsm->isInitialized;
}

void SatProcessAdapter::appendRevisions(const std::vector<RevisionData>& revisions, int desiredRevision, int nbThreads) {
    {
        auto lock = _mtx_revisions.getLock();
        _revisions_to_write.insert(_revisions_to_write.end(), revisions.begin(), revisions.end());
        _desired_revision = std::max(_desired_revision, desiredRevision);
        _num_revisions_to_write += revisions.size();
        for (auto& data : revisions) _sum_of_revision_sizes += data.fSize;
    }
    doWriteRevisions();
    _nb_threads = nbThreads;
    _thread_count_update = true;
}

void SatProcessAdapter::preregisterShmemObject(ShmemObject&& obj) {
    std::string shmemId = obj.id;
    _guard_prereg_shmem.lock().get()[shmemId] = std::move(obj);
}

void SatProcessAdapter::setSolvingState(SolvingStates::SolvingState state) {
    auto lock = _mtx_state.getLock();
    _state = state;
    if (!_initialized) return;
    applySolvingState();

}
void SatProcessAdapter::applySolvingState(bool initialize) {
    if (!initialize) assert(_initialized);
    if (_state == SolvingStates::ABORTING) {
        doTerminateInitializedProcess();
    }
    if (_state == SolvingStates::SUSPENDED || _state == SolvingStates::STANDBY) {
        Process::suspend(_child_pid); // Stop (suspend) process.
    }
    if (_state == SolvingStates::ACTIVE) {
        Process::resume(_child_pid); // Continue (resume) process.
    }
    if (initialize) _initialized.store(true, std::memory_order_release);
}

void SatProcessAdapter::doTerminateInitializedProcess() {
    _hsm->doTerminate = true; // Kindly ask child process to terminate.
}

void SatProcessAdapter::collectClauses(int maxSize) {
    if (!_initialized || _state != SolvingStates::ACTIVE || _clause_collecting_stage != NONE)
        return;
    _guard_pipe.lock().get()->writeData({maxSize}, CLAUSE_PIPE_PREPARE_CLAUSES);
    _clause_collecting_stage = QUERIED;
    if (_hsm->isInitialized) Process::wakeUp(_child_pid);
}
bool SatProcessAdapter::hasCollectedClauses() {
    return !_initialized || _state != SolvingStates::ACTIVE || _clause_collecting_stage == RETURNED;
}
std::vector<int> SatProcessAdapter::getCollectedClauses(int& successfulSolverId, int& numLits) {
    if (_clause_collecting_stage != RETURNED) return std::vector<int>();
    _clause_collecting_stage = NONE;
    successfulSolverId = _successful_solver_id;
    numLits = _nb_incoming_lits;
    return std::move(_collected_clauses);
}
int SatProcessAdapter::getLastAdmittedNumLits() {
    return _last_admitted_nb_lits;
}
long long SatProcessAdapter::getBestFoundObjectiveCost() const {
    return _best_found_objective_cost;
}
void SatProcessAdapter::updateBestFoundSolutionCost(long long bestFoundSolutionCost) {
    if (!_initialized || _state != SolvingStates::ACTIVE) return;
    _guard_pipe.lock().get()->writeData(
        {(int*) &bestFoundSolutionCost, (int*) ((&bestFoundSolutionCost)+1)},
        CLAUSE_PIPE_UPDATE_BEST_FOUND_OBJECTIVE_COST);
}

void SatProcessAdapter::filterClauses(int epoch, std::vector<int>&& clauses) {
    if (!_initialized || _state != SolvingStates::ACTIVE) return;
    _guard_pipe.lock().get()->writeData(std::move(clauses), {epoch},
        CLAUSE_PIPE_FILTER_IMPORT);
    _epoch_of_export_buffer = epoch;
    if (_hsm->isInitialized) Process::wakeUp(_child_pid);
}

bool SatProcessAdapter::hasFilteredClauses(int epoch) {
    if (!_initialized) return true; // will return dummy
    if (_state != SolvingStates::ACTIVE) return true; // may return dummy
    return _filters_by_epoch.count(epoch);
}

std::vector<int> SatProcessAdapter::getLocalFilter(int epoch) {
    std::vector<int> filter;
    auto it = _filters_by_epoch.find(epoch);
    if (it != _filters_by_epoch.end()) {
        filter = std::move(it->second);
        _filters_by_epoch.erase(it);
        assert(filter.size() >= (ClauseMetadata::enabled() ? 2 : 0));
        if (_epoch_of_export_buffer != epoch)
            filter.resize(ClauseMetadata::enabled() ? 2 : 0); // wrong epoch
    } else {
        filter = std::vector<int>(ClauseMetadata::enabled() ? 2 : 0, 0);
    }
    return filter;
}

void SatProcessAdapter::applyFilter(int epoch, std::vector<int>&& filter) {
    if (!_initialized || _state != SolvingStates::ACTIVE) return;
    if (epoch != _epoch_of_export_buffer) return; // ignore filter if the corresponding clauses are not present
    auto pipe = _guard_pipe.lock();
    if (*pipe) pipe.get()->writeData(std::move(filter), {epoch}, CLAUSE_PIPE_DIGEST_IMPORT);
    if (_hsm->isInitialized) Process::wakeUp(_child_pid);
}

void SatProcessAdapter::digestClausesWithoutFilter(int epoch, std::vector<int>&& clauses, bool stateless) {
    if (!_initialized || _state != SolvingStates::ACTIVE) return;
    _guard_pipe.lock().get()->writeData(std::move(clauses), {epoch, stateless?1:0},
        CLAUSE_PIPE_DIGEST_IMPORT_WITHOUT_FILTER);
    if (_hsm->isInitialized) Process::wakeUp(_child_pid);
}

void SatProcessAdapter::returnClauses(std::vector<int>&& clauses) {
    if (!_initialized || _state != SolvingStates::ACTIVE) return;
    _guard_pipe.lock().get()->writeData(std::move(clauses), {_clause_buffer_revision}, CLAUSE_PIPE_RETURN_CLAUSES);
}

void SatProcessAdapter::digestHistoricClauses(int epochBegin, int epochEnd, std::vector<int>&& clauses) {
    if (!_initialized || _state != SolvingStates::ACTIVE) return;
    _guard_pipe.lock().get()->writeData(std::move(clauses), {epochBegin, epochEnd, _clause_buffer_revision}, CLAUSE_PIPE_DIGEST_HISTORIC);
}


void SatProcessAdapter::dumpStats() {
    if (!_initialized || _state != SolvingStates::ACTIVE) return;
    _guard_pipe.lock().get()->writeData({}, CLAUSE_PIPE_DUMP_STATS);
    // No hard need to wake up immediately
}

SatProcessAdapter::SubprocessStatus SatProcessAdapter::check() {
    if (!_initialized) return NORMAL;

    int exitStatus = 0;
    if (!_hsm->doTerminate && !Terminator::isTerminating()
        && (_hsm->didTerminate ||
            (Process::didChildExit(_child_pid, &exitStatus) && exitStatus != 0))) {
        // Child has exited without being told to.
        if (exitStatus == SIGUSR2) {
            LOG(V3_VERB, "Restarting non-incremental child %ld\n", _child_pid);
        } else {
            LOG(V1_WARN, "[WARN] Child %ld exited unexpectedly (status %i)\n", _child_pid, exitStatus);
            if (!_params.restartSubprocessAtAbort()) {
                LOG(V0_CRIT, "[ERROR] Mallob is configured to abort together with the sub-process\n");
                abort();
            }
        }
        if (_params.proofOutputFile.isSet()) {
            // Certified UNSAT: Child crashing is not permitted!
            LOG(V1_WARN, "[ERROR] Child %ld exiting renders the proofs illegal - aborting\n", _child_pid);
            abort();
        }
        // Notify to restart solver engine
        return CRASHED;
    }

    doWriteRevisions();

    if (_state != SolvingStates::ACTIVE) return NORMAL;

    auto pipe = _guard_pipe.lock();
    char c = pipe.get()->pollForData();
    if (c == CLAUSE_PIPE_PREPARE_CLAUSES) {
        _collected_clauses = pipe.get()->readData(c);

        _successful_solver_id = _collected_clauses.back(); _collected_clauses.pop_back();
        _nb_incoming_lits = _collected_clauses.back(); _collected_clauses.pop_back();

        // read best found objective cost
        int costAsInts[sizeof(long long)/sizeof(int)];
        for (size_t i = 0; i < sizeof(long long)/sizeof(int); i++) {
            costAsInts[sizeof(long long)/sizeof(int) - 1 - i] = _collected_clauses.back();
            _collected_clauses.pop_back();
        }
        _best_found_objective_cost = * (long long*) costAsInts;
        if (_best_found_objective_cost != LLONG_MAX)
            LOG(V4_VVER, "best found objective cost: %lld\n", _best_found_objective_cost);

        _clause_collecting_stage = RETURNED;
        LOG(V5_DEBG, "collected clauses from subprocess\n");
    } else if (c == CLAUSE_PIPE_FILTER_IMPORT) {
        std::vector<int> filter = pipe.get()->readData(c);
        int epoch = filter.back(); filter.pop_back();
        _filters_by_epoch[epoch] = std::move(filter);
    } else if (c == CLAUSE_PIPE_DIGEST_IMPORT) {
        _last_admitted_nb_lits = pipe.get()->readData(c).front();
    } else if (c == CLAUSE_PIPE_SOLUTION) {
        std::vector<int> solution = pipe.get()->readData(c);
        pipe.unlock();
        const int resultCode = solution.back(); solution.pop_back();
        const unsigned long globalStartOfSuccessEpoch = * (unsigned long*) (solution.data()+solution.size()-2);
        solution.pop_back(); solution.pop_back();
        const int winningInstance = solution.back(); solution.pop_back();
        const int solutionRevision = solution.back(); solution.pop_back();
        if (solutionRevision == _desired_revision) {
            _solution.result = resultCode;
            _solution.revision = solutionRevision;
            _solution.winningInstanceId = winningInstance;
            _solution.globalStartOfSuccessEpoch = globalStartOfSuccessEpoch;
            _solution.setSolutionToSerialize(solution.empty() ? nullptr : solution.data(), solution.size());
            return FOUND_RESULT;
        }
    } else if (c == CLAUSE_PIPE_SUBMIT_PREPROCESSED_FORMULA) {
        _preprocessed_formula = pipe.get()->readData(c);
        pipe.unlock();
        return FOUND_PREPROCESSED_FORMULA;
    }
    pipe.unlock();

    if (_published_revision < _written_revision) {
        _published_revision = _written_revision;
        _guard_pipe.lock().get()->writeData({_desired_revision, _published_revision}, CLAUSE_PIPE_START_NEXT_REVISION);
    }

    if (_thread_count_update) {
        _guard_pipe.lock().get()->writeData({_nb_threads}, CLAUSE_PIPE_SET_THREAD_COUNT);
        _thread_count_update = false;
    }

    return NORMAL;
}

JobResult& SatProcessAdapter::getSolution() {
    return _solution;
}

void SatProcessAdapter::waitUntilChildExited() {
    if (!_running) return;
    while (!_initialized) { // make sure that there is a process to exit
        usleep(10*1000); // 10 ms
    }
    doTerminateInitializedProcess(); // make sure that the process receives a terminate signal
    while (!Process::didChildExit(_child_pid)) {
        Process::resume(_child_pid); // make sure that the process isn't frozen
        usleep(10*1000); // 10ms
    }
}

void* SatProcessAdapter::createSharedMemoryBlock(std::string shmemSubId, size_t size, const void* data, int rev, int descId, bool managedInCache) {
    if (size == 0) return (void*) 1;

    const std::string qualifiedShmemId = _shmem_id + "." + shmemSubId;
    std::string actualShmemId = qualifiedShmemId;

    void* shmem {nullptr};

    if (!managedInCache || descId == 0) {
        shmem = SharedMemory::create(qualifiedShmemId, size);
        assert(shmem);
        if (data) {
            memcpy(shmem, data, size);
        } else {
            memset(shmem, 0, size);
        }
        _shmem.insert(ShmemObject{actualShmemId, shmem, size,
            false, rev, qualifiedShmemId});
        return shmem;
    }

    actualShmemId = SharedMemoryCache::getShmemId(descId);
    {
        ShmemObject obj;
        auto preregShmem = _guard_prereg_shmem.lock();
        if (preregShmem->contains(actualShmemId)) {
            obj = std::move(preregShmem.get()[actualShmemId]);
            shmem = obj.data;
            preregShmem->erase(actualShmemId);
        }
        if (shmem)
            _shmem.insert(ShmemObject{actualShmemId, shmem, size,
                true, rev, obj.userLabel, descId});
    }
    if (!shmem) {
        shmem = StaticSharedMemoryCache::get().createOrAccess(descId, qualifiedShmemId, size,
            data, actualShmemId);
        _shmem.insert(ShmemObject{actualShmemId, shmem, size,
            true, rev, qualifiedShmemId, descId});
    }

    assert(shmem);
    auto descIdShmemId = "descid." + std::to_string(rev);
    createSharedMemoryBlock(descIdShmemId, sizeof(int), &descId);

    return shmem;
}

void SatProcessAdapter::crash() {
    _hsm->doCrash = true;
}

void SatProcessAdapter::reduceThreadCount() {
    if (!_initialized || _state != SolvingStates::ACTIVE) return;
    _guard_pipe.lock().get()->writeData({}, CLAUSE_PIPE_REDUCE_THREAD_COUNT);
}
void SatProcessAdapter::setThreadCount(int nbThreads) {
    if (!_initialized || _state != SolvingStates::ACTIVE) return;
    _guard_pipe.lock().get()->writeData({nbThreads}, CLAUSE_PIPE_SET_THREAD_COUNT);
}

SatProcessAdapter::~SatProcessAdapter() {
    freeSharedMemory();
}

void SatProcessAdapter::freeSharedMemory() {

    bool terminated = false;
    if (!_terminate.compare_exchange_strong(terminated, true)) {
        while (!_destructed) usleep(1000*1);
        return;
    }

    // wait for termination of background threads
    if (_bg_initializer.valid()) _bg_initializer.get();
    if (_bg_writer.valid()) _bg_writer.get();

    // delete shmem-based pipe
    _guard_pipe.lock()->reset();

    // Clean up shared memory objects created here
    _hsm = nullptr;
    {
        auto preregShmem = _guard_prereg_shmem.lock();
        for (auto& [id, obj] : preregShmem.get()) _shmem.insert(std::move(obj));
        preregShmem->clear();
    }
    for (auto& shmemObj : _shmem) {
        //log(V4_VVER, "DBG deleting %s\n", shmemObj.id.c_str());
        if (shmemObj.managedInCache) {
            const int descId = shmemObj.descId;
            assert(descId > 0);
            assert(shmemObj.data);
            StaticSharedMemoryCache::get().drop(descId, shmemObj.userLabel, shmemObj.size, shmemObj.data);
        } else {
            SharedMemory::free(shmemObj.id, (char*)shmemObj.data, shmemObj.size);
        }
    }
    _shmem.clear();
    _destructed = true;
}
