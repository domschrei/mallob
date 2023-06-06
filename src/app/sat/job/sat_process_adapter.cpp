
#include "app/sat/execution/solving_state.hpp"
#include "util/assert.hpp"
#include <atomic>
#include <sys/types.h>
#include <stdlib.h>
#include <fstream>
#include <cstdio>

#include "sat_process_adapter.hpp"

#include "../execution/engine.hpp"
#include "util/sys/shared_memory.hpp"
#include "util/sys/proc.hpp"
#include "util/sys/timer.hpp"
#include "util/sys/process.hpp"
#include "util/logger.hpp"
#include "comm/mympi.hpp"
#include "forked_sat_job.hpp"
#include "anytime_sat_clause_communicator.hpp"
#include "util/sys/thread_pool.hpp"
#include "util/sys/fileutils.hpp"

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
    _hsm->desiredRevision = _config.firstrev;
    _hsm->config = _config;

    // Allocate import and export buffers
    _hsm->exportBufferAllocatedSize = 2 * _params.maxSharingCompensationFactor() * _params.clauseBufferBaseSize() + 1024;
    _hsm->importBufferMaxSize = 2 * _params.maxSharingCompensationFactor() * MyMpi::getBinaryTreeBufferLimit(
        _hsm->config.mpisize, _params.clauseBufferBaseSize(), _params.clauseBufferLimitParam(),
        MyMpi::BufferQueryMode(_params.clauseBufferLimitMode())
    ) + 1024;
    _export_buffer = (int*) createSharedMemoryBlock("clauseexport", 
            sizeof(int)*_hsm->exportBufferAllocatedSize, nullptr);
    _import_buffer = (int*) createSharedMemoryBlock("clauseimport", 
            sizeof(int)*_hsm->importBufferMaxSize, nullptr);
    _filter_buffer = (int*) createSharedMemoryBlock("clausefilter", 
            _hsm->importBufferMaxSize/8 + 1, nullptr);
    _returned_buffer = (int*) createSharedMemoryBlock("returnedclauses",
            sizeof(int)*_hsm->importBufferMaxSize, nullptr);

    // Allocate shared memory for formula, assumptions of initial revision
    createSharedMemoryBlock("formulae.0", sizeof(int) * _f_size, (void*)_f_lits);
    createSharedMemoryBlock("assumptions.0", sizeof(int) * _a_size, (void*)_a_lits);

    if (_terminate) return;

    // FORK: Create a child process
    pid_t res = Process::createChild();
    if (res == 0) {
        // [child process]
        execl(MALLOB_SUBPROC_DISPATCH_PATH"mallob_process_dispatcher", 
              MALLOB_SUBPROC_DISPATCH_PATH"mallob_process_dispatcher", 
              (char*) 0);
        
        // If this is reached, something went wrong with execvp
        LOG(V0_CRIT, "[ERROR] execl returned errno %i\n", (int)errno);
        abort();
    }

    // Assemble SAT subprocess command
    std::string executable = MALLOB_SUBPROC_DISPATCH_PATH"mallob_sat_process";
    //char* const* argv = _params.asCArgs(executable.c_str());
    std::string command = _params.getSubprocCommandAsString(executable.c_str());
    
    // Write command to tmp file
    std::string commandOutfile = "/tmp/mallob_subproc_cmd_" + std::to_string(res) + "~";
    std::ofstream ofs(commandOutfile);
    ofs << command << " " << std::endl;
    ofs.close();
    std::rename(commandOutfile.c_str(), commandOutfile.substr(0, commandOutfile.size()-1).c_str()); // remove tilde

    //int i = 0;
    //delete[] ((const char**) argv);

    {
        auto lock = _state_mutex.getLock();
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
        _desired_revision = desiredRevision;
        _num_revisions_to_write += revisions.size();
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
        _pending_tasks.clear(); // delete pending tasks
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
    if (!_initialized) return;
    if (_hsm->doExport || _hsm->didExport) return;
    _hsm->exportBufferMaxSize = maxSize;
    _hsm->doExport = true;
    if (_hsm->isInitialized) Process::wakeUp(_child_pid);
}
bool SatProcessAdapter::hasCollectedClauses() {
    return !_initialized || (_hsm->doExport && _hsm->didExport);
}
std::vector<int> SatProcessAdapter::getCollectedClauses(int& successfulSolverId) {
    if (!_initialized || !hasCollectedClauses()) return std::vector<int>();
    assert(_hsm->exportBufferTrueSize <= _hsm->exportBufferAllocatedSize);
    std::vector<int> clauses(_export_buffer, _export_buffer+_hsm->exportBufferTrueSize);
    successfulSolverId = _hsm->successfulSolverId;
    _hsm->doExport = false;
    return clauses;
}
int SatProcessAdapter::getLastAdmittedNumLits() {
    return _last_admitted_nb_lits;
}

bool SatProcessAdapter::process(BufferTask& task) {

    if (!_initialized || _hsm->doFilterImport || _hsm->doDigestImportWithFilter 
            || _hsm->doReturnClauses || _hsm->doDigestImportWithoutFilter 
            || _hsm->doDigestHistoricClauses) {
        return false;
    }

    auto& buffer = task.payload;
    if (task.type == BufferTask::FILTER_CLAUSES) {
        _hsm->importBufferSize = buffer.size();
        _hsm->importBufferRevision = _desired_revision;
        _epoch_of_export_buffer = task.epoch;
        assert(_hsm->importBufferSize <= _hsm->importBufferMaxSize);
        memcpy(_import_buffer, buffer.data(), buffer.size()*sizeof(int));
        _hsm->winningSolverId = buffer.back();
        assert(_hsm->winningSolverId >= -1);
        _hsm->doFilterImport = true;
        _epochs_to_filter.erase(task.epoch);

    } else if (task.type == BufferTask::APPLY_FILTER) {
        if (_epoch_of_export_buffer == task.epoch) {
            memcpy(_filter_buffer, buffer.data(), buffer.size()*sizeof(int));
            _hsm->importEpoch = task.epoch;
            _hsm->doDigestImportWithFilter = true;
        } // else: discard this filter since the clauses are not present in any buffer

    } else if (task.type == BufferTask::RETURN_CLAUSES) {
        _hsm->returnedBufferSize = buffer.size();
        memcpy(_returned_buffer, buffer.data(),
            std::min((size_t)_hsm->importBufferMaxSize, buffer.size()) * sizeof(int));
        _hsm->doReturnClauses = true;

    } else if (task.type == BufferTask::DIGEST_CLAUSES_WITHOUT_FILTER) {
        _hsm->importBufferSize = buffer.size();
        _hsm->importBufferRevision = _desired_revision;
        _hsm->importEpoch = task.epoch;
        assert(_hsm->importBufferSize <= _hsm->importBufferMaxSize);
        memcpy(_import_buffer, buffer.data(), buffer.size()*sizeof(int));
        _hsm->doDigestImportWithoutFilter = true;

    } else if (task.type == BufferTask::DIGEST_HISTORIC_CLAUSES) {
        _hsm->historicEpochBegin = task.epoch;
        _hsm->historicEpochEnd = task.epochEnd;
        _hsm->importBufferSize = buffer.size();
        assert(_hsm->importBufferSize <= _hsm->importBufferMaxSize);
        memcpy(_import_buffer, buffer.data(), buffer.size()*sizeof(int));
        _hsm->doDigestHistoricClauses = true;
    }

    if (_hsm->isInitialized) Process::wakeUp(_child_pid);
    return true;
}

void SatProcessAdapter::tryProcessNextTasks() {
    if (!_initialized || _state == SolvingStates::ABORTING) return;
    while (!_pending_tasks.empty() && process(_pending_tasks.front())) {
        _pending_tasks.pop_front();
    }
}

void SatProcessAdapter::filterClauses(int epoch, const std::vector<int>& clauses) {
    getLocalFilter(-1); // fetch any previous, old result
    _epochs_to_filter.insert(epoch);
    _pending_tasks.emplace_back(BufferTask{BufferTask::FILTER_CLAUSES, clauses, epoch});
    tryProcessNextTasks();
}

bool SatProcessAdapter::hasFilteredClauses(int epoch) {
    if (!_initialized) return true; // will return dummy
    if (_state != SolvingStates::ACTIVE) return true; // may return dummy
    if (_epochs_to_filter.count(epoch)) {
        return false; // filtering task still in processing queue, job is active
    }
    return _hsm->doFilterImport && _hsm->didFilterImport;
}

std::vector<int> SatProcessAdapter::getLocalFilter(int epoch) {
    std::vector<int> filter;
    if (_initialized && _hsm->doFilterImport && _hsm->didFilterImport) {
        filter.resize(_hsm->filterSize);
        memcpy(filter.data(), _filter_buffer, _hsm->filterSize*sizeof(int));
        _hsm->doFilterImport = false;
        assert(filter.size() >= ClauseMetadata::numBytes());
        if (_epoch_of_export_buffer != epoch)
            filter.resize(ClauseMetadata::numBytes()); // wrong epoch
    } else {
        filter = std::vector<int>(ClauseMetadata::numBytes(), 0);
    }
    return filter;
}

void SatProcessAdapter::applyFilter(int epoch, const std::vector<int>& filter) {
    _pending_tasks.emplace_back(BufferTask{BufferTask::APPLY_FILTER, filter, epoch});
    tryProcessNextTasks();
}

void SatProcessAdapter::returnClauses(const std::vector<int>& clauses) {
    _pending_tasks.emplace_back(BufferTask{BufferTask::RETURN_CLAUSES, clauses, -1});
    tryProcessNextTasks();
}

void SatProcessAdapter::digestHistoricClauses(int epochBegin, int epochEnd, const std::vector<int>& clauses) {
    _pending_tasks.emplace_back(BufferTask{BufferTask::DIGEST_HISTORIC_CLAUSES, clauses, epochBegin, epochEnd});
    tryProcessNextTasks();
}

void SatProcessAdapter::digestClausesWithoutFilter(const std::vector<int>& clauses) {
    _pending_tasks.emplace_back(BufferTask{BufferTask::DIGEST_CLAUSES_WITHOUT_FILTER, clauses, -1});
    tryProcessNextTasks();
}


void SatProcessAdapter::dumpStats() {
    if (!_initialized) return;
    _hsm->doDumpStats = true;
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
        }
        if (ClauseMetadata::enabled()) {
            // Certified UNSAT: Child crashing is not permitted!
            LOG(V1_WARN, "[ERROR] Child %ld exiting renders the proofs illegal - aborting\n", _child_pid);
            abort();
        }
        // Notify to restart solver engine
        return CRASHED;
    }

    doWriteRevisions();

    if (_hsm->didReturnClauses)         _hsm->doReturnClauses         = false;
    if (_hsm->didDigestHistoricClauses) _hsm->doDigestHistoricClauses = false;
    if (_hsm->didStartNextRevision)     _hsm->doStartNextRevision     = false;
    if (_hsm->didDumpStats)             _hsm->doDumpStats             = false;
    if (_hsm->didReduceThreadCount)     _hsm->doReduceThreadCount     = false;
    if (_hsm->didDigestImport) {
        _hsm->doDigestImportWithFilter = false;
        _hsm->doDigestImportWithoutFilter = false;
        _last_admitted_nb_lits = _hsm->lastAdmittedStats.nbAdmittedLits;
    }

    if (!_hsm->doStartNextRevision 
        && !_hsm->didStartNextRevision 
        && _published_revision < _written_revision) {
        _published_revision++;
        _hsm->desiredRevision = _desired_revision;
        _hsm->doStartNextRevision = true;
    }

    tryProcessNextTasks();

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
    if (!_hsm->doReduceThreadCount && !_hsm->didReduceThreadCount)
        _hsm->doReduceThreadCount = true;
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
