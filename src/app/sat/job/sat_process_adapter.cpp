
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
#include "app/sat/job/inplace_sharing_aggregation.hpp"
#include "sat_process_adapter.hpp"
#include "../execution/engine.hpp"
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
        _f_size(fSize), _f_lits(fLits), _a_size(aSize), _a_lits(aLits) {

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
            createSharedMemoryBlock("formulae."    + revStr, sizeof(int) * revData.fSize, (void*)revData.fLits);
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
    createSharedMemoryBlock("formulae.0", sizeof(int) * _f_size, (void*)_f_lits);
    createSharedMemoryBlock("assumptions.0", sizeof(int) * _a_size, (void*)_a_lits);

    // Set up bi-directional pipe to and from the subprocess
    _pipe.reset(new BiDirectionalAnytimePipe(BiDirectionalAnytimePipe::CREATE,
        TmpDir::get()+_shmem_id+".tosub.pipe",
        TmpDir::get()+_shmem_id+".fromsub.pipe",
        &_hsm->childReadyToWrite));

    if (_terminate) return;

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

void SatProcessAdapter::setSolvingState(SolvingStates::SolvingState state) {
    auto lock = _state_mutex.getLock();
    _state = state;
    if (!_initialized) return;
    applySolvingState();

}
void SatProcessAdapter::applySolvingState() {
    assert(_initialized && _child_pid != -1);
    if (_state == SolvingStates::ABORTING && _hsm != nullptr) {
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
    //Fork::terminate(_child_pid); // Terminate child process by signal.
    _hsm->doTerminate = true; // Kindly ask child process to terminate.
    _hsm->doBegin = true; // Let child process know termination even if it waits for first revision
    Process::resume(_child_pid); // Continue (resume) process.
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
        assert(filter.size() >= ClauseMetadata::enabled() ? 2 : 0);
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

void SatProcessAdapter::digestClausesWithoutFilter(int epoch, const std::vector<int>& clauses) {
    if (!_initialized || _state != SolvingStates::ACTIVE) return;
    _pipe->writeData(clauses, {epoch}, CLAUSE_PIPE_DIGEST_IMPORT_WITHOUT_FILTER);
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
            Process::didChildExit(_child_pid, &exitStatus) && exitStatus != 0)) {
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
    }

    if (_published_revision < _written_revision) {
        _published_revision = _written_revision;
        _pipe->writeData({_desired_revision, _published_revision}, CLAUSE_PIPE_START_NEXT_REVISION);
    }

    // Solution preparation just ended?
    if (!_solution_in_preparation && _solution_prepare_future.valid()) {
        _solution_prepare_future.get();
    }
    if (_hsm->hasSolution && _hsm->solutionRevision == _desired_revision) {
        // Preparation still going on?
        if (_solution_in_preparation) return NORMAL;
        // Correct solution prepared successfully?
        if (_solution_revision_in_preparation == _desired_revision)
            return FOUND_RESULT;
        // No preparation going on yet?
        if (!_solution_prepare_future.valid()) {
            // Begin preparation of solution
            _solution_revision_in_preparation = _desired_revision;
            _solution_in_preparation = true;
            if (_sum_of_revision_sizes <= 500'000) {
                // Small job: Extract solution immediately in this thread.
                doPrepareSolution();
                return FOUND_RESULT;
            }
            // Large job: Extract solution concurrently
            _solution_prepare_future = ProcessWideThreadPool::get().addTask([&]() {
                doPrepareSolution();
            });
        }
    } 
    return NORMAL;
}

void SatProcessAdapter::doPrepareSolution() {

    int rev = _solution_revision_in_preparation;
    size_t* solutionSize = (size_t*) SharedMemory::access(_shmem_id + ".solutionsize." + std::to_string(rev), sizeof(size_t));
    if (*solutionSize == 0) {
        _solution.result = _hsm->result;
        _solution.winningInstanceId = _hsm->winningInstance;
        _solution.globalStartOfSuccessEpoch = _hsm->globalStartOfSuccessEpoch;
        _solution.setSolutionToSerialize(nullptr, 0);
        _solution_in_preparation = false;
        return;
    } 

    // ACCESS the existing shared memory segment to the solution vector
    int* shmemSolution = (int*) SharedMemory::access(_shmem_id + ".solution." + std::to_string(rev), *solutionSize*sizeof(int));
    
    _solution.result = _hsm->result;
    _solution.winningInstanceId = _hsm->winningInstance;
    _solution.setSolutionToSerialize(shmemSolution, *solutionSize);
    _solution_in_preparation = false;
}

JobResult& SatProcessAdapter::getSolution() {
    return _solution;
}

void SatProcessAdapter::waitUntilChildExited() {
    if (!_running) return;
    // Wait until initialized
    while (!_initialized) {
        usleep(100*1000); // 0.1s
    }
    doTerminateInitializedProcess(); // make sure that the process receives a terminate signal
    _pipe.reset(); // clean up bidirectional pipe
    while (true) {
        // Check if child exited
        auto lock = _state_mutex.getLock();
        if (_child_pid == -1 || Process::didChildExit(_child_pid)) 
            return;
        usleep(100*1000); // 0.1s
    }
}

void* SatProcessAdapter::createSharedMemoryBlock(std::string shmemSubId, size_t size, void* data) {
    std::string id = _shmem_id + "." + shmemSubId;
    void* shmem = SharedMemory::create(id, size);
    if (data == nullptr) {
        memset(shmem, 0, size);
    } else {
        memcpy(shmem, data, size);
    }
    _shmem.insert(ShmemObject{id, shmem, size});
    //log(V4_VVER, "DBG set up shmem %s\n", id.c_str());
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
    
    if (!_terminate) {
        _terminate = true;

        // wait for termination of background threads
        if (_bg_initializer.valid()) _bg_initializer.get();
        if (_bg_writer.valid()) _bg_writer.get();
        if (_solution_prepare_future.valid()) _solution_prepare_future.get();
    }

    // Clean up found solutions in shared memory
    if (_hsm != nullptr) {
        for (int rev = 0; rev <= _written_revision; rev++) {
            size_t* solSize = (size_t*) SharedMemory::access(_shmem_id + ".solutionsize." + std::to_string(rev), sizeof(size_t));
            if (solSize != nullptr) {
                char* solution = (char*) SharedMemory::access(_shmem_id + ".solution." + std::to_string(rev), *solSize * sizeof(int));
                SharedMemory::free(_shmem_id + ".solution." + std::to_string(rev), solution, *solSize * sizeof(int));
                SharedMemory::free(_shmem_id + ".solutionsize." + std::to_string(rev), (char*)solSize, sizeof(size_t));
            }
        }
        _hsm = nullptr;
    }

    // Clean up shared memory objects created here
    for (auto& shmemObj : _shmem) {
        //log(V4_VVER, "DBG deleting %s\n", shmemObj.id.c_str());
        SharedMemory::free(shmemObj.id, (char*)shmemObj.data, shmemObj.size);
    }
    _shmem.clear();
}
