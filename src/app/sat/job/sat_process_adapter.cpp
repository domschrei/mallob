
#include <stdlib.h>
#include <assert.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <atomic>
#include <functional>
#include <new>
#include <utility>

#include "app/sat/data/clause_metadata.hpp"
#include "app/sat/execution/clause_pipe_defs.hpp"
#include "app/sat/execution/solving_state.hpp"
#include "app/sat/job/forked_sat_job.hpp"
#include "app/sat/job/formula_shmem_cache.hpp"
#include "app/sat/job/inplace_sharing_aggregation.hpp"
#include "sat_process_adapter.hpp"
#include "../execution/engine.hpp"
#include "util/string_utils.hpp"
#include "util/sys/bidirectional_anytime_pipe.hpp"
#include "util/sys/shared_memory.hpp"
#include "util/sys/proc.hpp"
#include "util/sys/subprocess.hpp"
#include "util/sys/process.hpp"
#include "util/logger.hpp"
#include "util/sys/thread_pool.hpp"
#include "app/sat/job/sat_shared_memory.hpp"
#include "util/option.hpp"
#include "util/sys/tmpdir.hpp"

#ifndef MALLOB_SUBPROC_DISPATCH_PATH
#define MALLOB_SUBPROC_DISPATCH_PATH ""
#endif

SatProcessAdapter::SatProcessAdapter(Parameters&& params, SatProcessConfig&& config, ForkedSatJob* job,
    size_t fSize, const int* fLits, size_t aSize, const int* aLits, 
    std::shared_ptr<AnytimeSatClauseCommunicator>& comm) :    
        _params(std::move(params)), _config(std::move(config)), _job(job), _clause_comm(comm),
        _f_size(fSize), _f_lits(
            _job->getDescription().isRevisionIncomplete(0) ? nullptr : fLits
        ), _a_size(aSize), _a_lits(aLits) {

    _desired_revision = _config.firstrev;
    _shmem_id = _config.getSharedMemId(Proc::getPid());
    assert(_clause_comm);
}

void SatProcessAdapter::doWriteRevisions() {

    if (_num_revisions_to_write.load(std::memory_order_relaxed) == 0) return;
    if (!_initialized || _hsm->doTerminate || !_revisions_mutex.tryLock()) return;

    if (_bg_writer_running) {
        _revisions_mutex.unlock();
        return;
    }
    if (_bg_writer.valid()) _bg_writer.get();
    
    _bg_writer = ProcessWideThreadPool::get().addTask([this]() {
        while (!_terminate && _num_revisions_to_write > 0) {
            RevisionData revData;
            {
                auto lock = _revisions_mutex.getLock();
                if (_revisions_to_write.empty()) break;
                revData = _revisions_to_write.front();
                _revisions_to_write.erase(_revisions_to_write.begin());
                _num_revisions_to_write--;
            }
            LOG(V4_VVER, "DBG Writing next revision\n");
            auto revStr = std::to_string(revData.revision);
            createSharedMemoryBlock("fsize."       + revStr, sizeof(size_t),              (void*)&revData.fSize);
            createSharedMemoryBlock("asize."       + revStr, sizeof(size_t),              (void*)&revData.aSize);
            const int* fPtr = (const int*) createSharedMemoryBlock("formulae." + revStr, sizeof(int) * revData.fSize, (void*)revData.fLits, revData.revision, true);
            LOG(V2_INFO, "SUMMARY %s\n", StringUtils::getSummary(fPtr, revData.fSize).c_str());
            if (revData.fSize > 0) assert(fPtr[0] != 0);
            if (revData.fSize > 0) assert(fPtr[revData.fSize-1] == 0);
            createSharedMemoryBlock("assumptions." + revStr, sizeof(int) * revData.aSize, (void*)revData.aLits);
            createSharedMemoryBlock("checksum."    + revStr, sizeof(Checksum),            (void*)&(revData.checksum));
            _written_revision = revData.revision;
            LOG(V4_VVER, "DBG Done writing next revision %i\n", revData.revision);
        }
        auto lock = _revisions_mutex.getLock();
        _bg_writer_running = false;
    });

    _bg_writer_running = true;
    _revisions_mutex.unlock();
}

void SatProcessAdapter::run() {
    _running = true;
    _bg_initializer = ProcessWideThreadPool::get().addTask(
        std::bind(&SatProcessAdapter::doInitialize, this)
    );
}

void SatProcessAdapter::doInitialize() {

    // Initialize "management" shared memory
    //log(V4_VVER, "Setup base shmem: %s\n", _shmem_id.c_str());
    void* mainShmem = SharedMemory::create(_shmem_id, sizeof(SatSharedMemory));
    _shmem.insert(ShmemObject{_shmem_id, mainShmem, sizeof(SatSharedMemory)});
    // "placement new" operator: construct object not in the heap but in the provided chunk of memory
    _hsm = new ((char*)mainShmem) SatSharedMemory();
    _hsm->fSize = _f_size;
    _hsm->aSize = _a_size;
    _hsm->config = _config;
    _sum_of_revision_sizes += _f_size;

    // Allocate shared memory for formula, assumptions of initial revision
    const int* fInShmem = (const int*) createSharedMemoryBlock("formulae.0", sizeof(int) * _f_size, (void*)_f_lits, 0, true);
    LOG(V2_INFO, "SUMMARY %s\n", StringUtils::getSummary(fInShmem, _f_size).c_str());
    if (_f_size > 0) assert(fInShmem[0] != 0);
    if (_f_size > 0) assert(fInShmem[_f_size-1] == 0);
    createSharedMemoryBlock("assumptions.0", sizeof(int) * _a_size, (void*)_a_lits);

    // Set up bi-directional pipe to and from the subprocess
    _pipe.reset(new BiDirectionalAnytimePipe(BiDirectionalAnytimePipe::CREATE,
        TmpDir::getGeneralTmpDir()+_shmem_id+".tosub.pipe",
        TmpDir::getGeneralTmpDir()+_shmem_id+".fromsub.pipe",
        &_hsm->childReadyToWrite));

    // Create SAT solving child process
    Subprocess subproc(_params, "mallob_sat_process");
    pid_t res = subproc.start();

    {
        auto lock = _state_mutex.getLock();
        _pipe->open();
        _initialized = true;
        _hsm->doBegin = true;
        _child_pid = res;
        _state = SolvingStates::ACTIVE;
        applySolvingState();
    }
}

bool SatProcessAdapter::isFullyInitialized() {
    return _initialized && _hsm->isInitialized;
}

void SatProcessAdapter::appendRevisions(const std::vector<RevisionData>& revisions, int desiredRevision) {
    {
        auto lock = _revisions_mutex.getLock();
        _revisions_to_write.insert(_revisions_to_write.end(), revisions.begin(), revisions.end());
        _desired_revision = std::max(_desired_revision, desiredRevision);
        _num_revisions_to_write += revisions.size();
        for (auto& data : revisions) _sum_of_revision_sizes += data.fSize;
    }
    doWriteRevisions();
}

void SatProcessAdapter::preregisterShmemObject(ShmemObject&& obj) {
    std::string shmemId = obj.id;
    auto lock = _mtx_preregistered_shmem.getLock();
    _preregistered_shmem[shmemId] = std::move(obj);
}

void SatProcessAdapter::setSolvingState(SolvingStates::SolvingState state) {
    auto lock = _state_mutex.getLock();
    _state = state;
    if (!_initialized) return;
    applySolvingState();

}
void SatProcessAdapter::applySolvingState() {
    assert(_initialized && _child_pid != -1);
    if (_state == SolvingStates::ABORTING) {
        doTerminateInitializedProcess();
    }
    if (_state == SolvingStates::SUSPENDED || _state == SolvingStates::STANDBY) {
        Process::suspend(_child_pid); // Stop (suspend) process.
    }
    if (_state == SolvingStates::ACTIVE) {
        Process::resume(_child_pid); // Continue (resume) process.
    }
}

void SatProcessAdapter::doTerminateInitializedProcess() {
    if (!_running || _hsm->doTerminate) return; // running?
    while (!_initialized) usleep(10*1000); // wait until initialized
    Process::resume(_child_pid); // Continue (resume) process.
    while (!_hsm->didBegin) usleep(10*1000); // wait until child is actually in the main loop
    _hsm->doTerminate = true; // Kindly ask child process to terminate.
    _pipe.reset(); // clean up bidirectional pipe
}

void SatProcessAdapter::collectClauses(int maxSize) {
    if (!_initialized || _state != SolvingStates::ACTIVE || _clause_collecting_stage != NONE)
        return;
    _pipe->writeData({maxSize}, CLAUSE_PIPE_PREPARE_CLAUSES);
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

void SatProcessAdapter::filterClauses(int epoch, const std::vector<int>& clauses) {
    if (!_initialized || _state != SolvingStates::ACTIVE) return;
    _pipe->writeData(clauses, {epoch},
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

void SatProcessAdapter::applyFilter(int epoch, const std::vector<int>& filter) {
    if (!_initialized || _state != SolvingStates::ACTIVE) return;
    if (epoch != _epoch_of_export_buffer) return; // ignore filter if the corresponding clauses are not present
    _pipe->writeData(filter, {epoch}, CLAUSE_PIPE_DIGEST_IMPORT);
    if (_hsm->isInitialized) Process::wakeUp(_child_pid);
}

void SatProcessAdapter::digestClausesWithoutFilter(int epoch, const std::vector<int>& clauses, bool stateless) {
    if (!_initialized || _state != SolvingStates::ACTIVE) return;
    _pipe->writeData(clauses, {epoch, stateless?1:0},
        CLAUSE_PIPE_DIGEST_IMPORT_WITHOUT_FILTER);
    if (_hsm->isInitialized) Process::wakeUp(_child_pid);
}

void SatProcessAdapter::returnClauses(const std::vector<int>& clauses) {
    if (!_initialized || _state != SolvingStates::ACTIVE) return;
    _pipe->writeData(clauses, {_clause_buffer_revision}, CLAUSE_PIPE_RETURN_CLAUSES);
}

void SatProcessAdapter::digestHistoricClauses(int epochBegin, int epochEnd, const std::vector<int>& clauses) {
    if (!_initialized || _state != SolvingStates::ACTIVE) return;
    _pipe->writeData(clauses, {epochBegin, epochEnd, _clause_buffer_revision}, CLAUSE_PIPE_DIGEST_HISTORIC);
}


void SatProcessAdapter::dumpStats() {
    if (!_initialized || _state != SolvingStates::ACTIVE) return;
    _pipe->writeData({}, CLAUSE_PIPE_DUMP_STATS);
    // No hard need to wake up immediately
}

SatProcessAdapter::SubprocessStatus SatProcessAdapter::check() {
    if (!_initialized) return NORMAL;

    int exitStatus = 0;
    if (!_hsm->doTerminate && (_hsm->didTerminate ||
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

    char c = _pipe->pollForData();
    if (c == CLAUSE_PIPE_PREPARE_CLAUSES) {
        _collected_clauses = _pipe->readData(c);
        _successful_solver_id = _collected_clauses.back(); _collected_clauses.pop_back();
        _nb_incoming_lits = _collected_clauses.back(); _collected_clauses.pop_back();
        _clause_collecting_stage = RETURNED;
        LOG(V4_VVER, "collected clauses from subprocess\n");
    } else if (c == CLAUSE_PIPE_FILTER_IMPORT) {
        std::vector<int> filter = _pipe->readData(c);
        int epoch = filter.back(); filter.pop_back();
        _filters_by_epoch[epoch] = std::move(filter);
    } else if (c == CLAUSE_PIPE_DIGEST_IMPORT) {
        _last_admitted_nb_lits = _pipe->readData(c).front();
    } else if (c == CLAUSE_PIPE_SOLUTION) {
        std::vector<int> solution = _pipe->readData(c);
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
    }

    if (_published_revision < _written_revision) {
        _published_revision = _written_revision;
        _pipe->writeData({_desired_revision, _published_revision}, CLAUSE_PIPE_START_NEXT_REVISION);
    }

    return NORMAL;
}

JobResult& SatProcessAdapter::getSolution() {
    return _solution;
}

void SatProcessAdapter::waitUntilChildExited() {
    doTerminateInitializedProcess(); // make sure that the process receives a terminate signal
    while (true) {
        // Check if child exited
        auto lock = _state_mutex.getLock();
        if (_child_pid == -1 || Process::didChildExit(_child_pid)) 
            return;
        usleep(100*1000); // 0.1s
    }
}

void* SatProcessAdapter::createSharedMemoryBlock(std::string shmemSubId, size_t size, const void* data, int rev, bool managedInCache) {
    if (size == 0) return (void*) 1;

    const std::string qualifiedShmemId = _shmem_id + "." + shmemSubId;
    std::string actualShmemId = qualifiedShmemId;
    const int descId = _job->getDescription().getJobDescriptionId(rev);

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

    actualShmemId = FormulaSharedMemoryCache::getShmemId(descId);
    {
        ShmemObject obj;
        auto lock = _mtx_preregistered_shmem.getLock();
        if (_preregistered_shmem.contains(actualShmemId)) {
            obj = std::move(_preregistered_shmem[actualShmemId]);
            shmem = obj.data;
            _preregistered_shmem.erase(actualShmemId);
        }
        if (shmem)
            _shmem.insert(ShmemObject{actualShmemId, shmem, size,
                true, rev, obj.userLabel});
    }
    if (!shmem) {
        shmem = StaticFormulaSharedMemoryCache::get().createOrAccess(descId, qualifiedShmemId, size,
            data, actualShmemId);
        _shmem.insert(ShmemObject{actualShmemId, shmem, size,
            true, rev, qualifiedShmemId});
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
    _pipe->writeData({}, CLAUSE_PIPE_REDUCE_THREAD_COUNT);
}

SatProcessAdapter::~SatProcessAdapter() {
    freeSharedMemory();
}

void SatProcessAdapter::freeSharedMemory() {

    bool terminated = false;
    if (!_terminate.compare_exchange_strong(terminated, true)) {
        while (!_destructed) usleep(1000*10);
        return;
    }

    // wait for termination of background threads
    if (_bg_initializer.valid()) _bg_initializer.get();
    if (_bg_writer.valid()) _bg_writer.get();

    // Clean up shared memory objects created here
    _hsm = nullptr;
    {
        auto lock = _mtx_preregistered_shmem.getLock();
        for (auto& [id, obj] : _preregistered_shmem) _shmem.insert(std::move(obj));
        _preregistered_shmem.clear();
    }
    for (auto& shmemObj : _shmem) {
        //log(V4_VVER, "DBG deleting %s\n", shmemObj.id.c_str());
        if (shmemObj.managedInCache) {
            const int descId = _job->getDescription().getJobDescriptionId(shmemObj.revision);
            assert(descId > 0);
            assert(shmemObj.data);
            StaticFormulaSharedMemoryCache::get().drop(descId, shmemObj.userLabel, shmemObj.size, shmemObj.data);
        } else {
            SharedMemory::free(shmemObj.id, (char*)shmemObj.data, shmemObj.size);
        }
    }
    _shmem.clear();
    _destructed = true;
}
