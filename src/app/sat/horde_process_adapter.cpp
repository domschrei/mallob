
#include "util/assert.hpp"
#include <sys/types.h>
#include <stdlib.h>

#include "horde_process_adapter.hpp"

#include "hordesat/horde.hpp"
#include "util/sys/shared_memory.hpp"
#include "util/sys/proc.hpp"
#include "util/sys/timer.hpp"
#include "util/sys/process.hpp"
#include "util/logger.hpp"
#include "comm/mympi.hpp"
#include "app/sat/forked_sat_job.hpp"
#include "app/sat/anytime_sat_clause_communicator.hpp"
#include "util/sys/thread_pool.hpp"

HordeProcessAdapter::HordeProcessAdapter(Parameters&& params, HordeConfig&& config, ForkedSatJob* job,
    size_t fSize, const int* fLits, size_t aSize, const int* aLits, AnytimeSatClauseCommunicator* comm) :    
        _params(std::move(params)), _config(std::move(config)), _job(job), _clause_comm(comm),
        _f_size(fSize), _f_lits(fLits), _a_size(aSize), _a_lits(aLits) {

    _desired_revision = _config.firstrev;
    _shmem_id = _config.getSharedMemId(Proc::getPid());
}

void HordeProcessAdapter::doWriteRevisions() {

    auto task = [this]() {
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
    };

    if (!_initialized || !_revisions_mutex.tryLock()) return;
    if (!_bg_writer_running) {
        if (_bg_writer.valid()) _bg_writer.get();
        _bg_writer = ProcessWideThreadPool::get().addTask(task);
        _bg_writer_running = true;
    }
    _revisions_mutex.unlock();
}

void HordeProcessAdapter::run() {
    _running = true;
    _bg_initializer = ProcessWideThreadPool::get().addTask(
        std::bind(&HordeProcessAdapter::doInitialize, this)
    );
}

void HordeProcessAdapter::doInitialize() {

    if (_clause_comm == nullptr)
        _clause_comm = new AnytimeSatClauseCommunicator(_params, _job);

    // Initialize "management" shared memory
    //log(V4_VVER, "Setup base shmem: %s\n", _shmem_id.c_str());
    void* mainShmem = SharedMemory::create(_shmem_id, sizeof(HordeSharedMemory));
    _shmem.insert(ShmemObject{_shmem_id, mainShmem, sizeof(HordeSharedMemory)});
    _hsm = new ((char*)mainShmem) HordeSharedMemory();
    _hsm->doBegin = false;
    _hsm->doExport = false;
    _hsm->doImport = false;
    _hsm->doReturnClauses = false;
    _hsm->doDumpStats = false;
    _hsm->doStartNextRevision = false;
    _hsm->doTerminate = false;
    _hsm->exportBufferMaxSize = 0;
    _hsm->importBufferSize = 0;
    _hsm->didExport = false;
    _hsm->didImport = false;
    _hsm->didReturnClauses = false;
    _hsm->didDumpStats = false;
    _hsm->didStartNextRevision = false;
    _hsm->didTerminate = false;
    _hsm->isInitialized = false;
    _hsm->hasSolution = false;
    _hsm->result = UNKNOWN;
    _hsm->solutionRevision = -1;
    _hsm->exportBufferTrueSize = 0;
    _hsm->fSize = _f_size;
    _hsm->aSize = _a_size;
    _hsm->desiredRevision = _config.firstrev;
    _hsm->config = _config;

    // Allocate import and export buffers
    _export_buffer = (int*) createSharedMemoryBlock("clauseexport", 
            _params.clauseBufferBaseSize() * sizeof(int), nullptr);
    _hsm->importBufferMaxSize = _params.clauseHistoryAggregationFactor() * MyMpi::getBinaryTreeBufferLimit(
        _hsm->config.mpisize, _params.clauseBufferBaseSize(), _params.clauseBufferDiscountFactor(), 
        MyMpi::ALL
    ) + 1024;
    _import_buffer = (int*) createSharedMemoryBlock("clauseimport", 
            sizeof(int)*_hsm->importBufferMaxSize, nullptr);
    _returned_buffer = (int*) createSharedMemoryBlock("returnedclauses",
            sizeof(int)*_hsm->importBufferMaxSize, nullptr);

    // Allocate shared memory for formula, assumptions of initial revision
    createSharedMemoryBlock("formulae.0", sizeof(int) * _f_size, (void*)_f_lits);
    createSharedMemoryBlock("assumptions.0", sizeof(int) * _a_size, (void*)_a_lits);

    if (_terminate) return;

    // Assemble c-style program arguments
    char* const* argv = _params.asCArgs("mallob_sat_process");

    // FORK: Create a child process
    pid_t res = Process::createChild();
    if (res == 0) {
        // [child process]
        // Execute the SAT process.
        int result = execvp(argv[0], argv);
        
        // If this is reached, something went wrong with execvp
        LOG(V0_CRIT, "[ERROR] execvp returned %i with errno %i\n", result, (int)errno);
        abort();
    }

    // [parent process]
    int i = 0;
    while (argv[i] != nullptr) free(argv[i++]);
    delete[] argv;

    {
        auto lock = _state_mutex.getLock();
        _initialized = true;
        _hsm->doBegin = true;
        _child_pid = res;
        applySolvingState();
    }
}

bool HordeProcessAdapter::hasClauseComm() {
    return _initialized;
}

bool HordeProcessAdapter::isFullyInitialized() {
    return _initialized && _hsm->isInitialized;
}

void HordeProcessAdapter::appendRevisions(const std::vector<RevisionData>& revisions, int desiredRevision) {
    {
        auto lock = _revisions_mutex.getLock();
        _revisions_to_write.insert(_revisions_to_write.end(), revisions.begin(), revisions.end());
        _desired_revision = desiredRevision;
        _num_revisions_to_write += revisions.size();
    }
    doWriteRevisions();
}

void HordeProcessAdapter::setSolvingState(SolvingStates::SolvingState state) {
    auto lock = _state_mutex.getLock();
    _state = state;
    if (!_initialized) return;
    applySolvingState();

}
void HordeProcessAdapter::applySolvingState() {
    assert(_initialized && _child_pid != -1);
    if (_state == SolvingStates::ABORTING && _hsm != nullptr) {
        //Fork::terminate(_child_pid); // Terminate child process by signal.
        _hsm->doTerminate = true; // Kindly ask child process to terminate.
        _hsm->doBegin = true; // Let child process know termination even if it waits for first revision
        Process::resume(_child_pid); // Continue (resume) process.
    }
    if (_state == SolvingStates::SUSPENDED || _state == SolvingStates::STANDBY) {
        Process::suspend(_child_pid); // Stop (suspend) process.
        ((AnytimeSatClauseCommunicator*)_clause_comm)->suspend();
    }
    if (_state == SolvingStates::ACTIVE) {
        Process::resume(_child_pid); // Continue (resume) process.
    }
}

void HordeProcessAdapter::collectClauses(int maxSize) {
    if (!_initialized) return;
    if (_hsm->doExport) return;
    _hsm->exportBufferMaxSize = maxSize;
    _hsm->doExport = true;
    if (_hsm->isInitialized) Process::wakeUp(_child_pid);
}
bool HordeProcessAdapter::hasCollectedClauses() {
    return _initialized && _hsm->didExport;
}
std::vector<int> HordeProcessAdapter::getCollectedClauses(Checksum& checksum) {
    if (!_initialized || !_hsm->didExport) {
        return std::vector<int>();
    }
    std::vector<int> clauses(_export_buffer, _export_buffer+_hsm->exportBufferTrueSize);
    checksum = _hsm->exportChecksum;
    _hsm->doExport = false;
    return clauses;
}

void HordeProcessAdapter::digestClauses(const std::vector<int>& clauses, const Checksum& checksum) {
    if (!_initialized || _hsm->doImport) {
        // Cannot import right now -- defer into temporary storage 
        _temp_clause_buffers.emplace_back(clauses, checksum);
        return;
    }
    doDigest(clauses, checksum);
}

void HordeProcessAdapter::doDigest(const std::vector<int>& clauses, const Checksum& checksum) {
    _hsm->importChecksum = checksum;
    _hsm->importBufferSize = clauses.size();
    _hsm->importBufferRevision = _desired_revision;
    assert(_hsm->importBufferSize <= _hsm->importBufferMaxSize);
    memcpy(_import_buffer, clauses.data(), clauses.size()*sizeof(int));
    _hsm->doImport = true;
    if (_hsm->isInitialized) Process::wakeUp(_child_pid);
}

void HordeProcessAdapter::returnClauses(const std::vector<int>& clauses) {
    if (!_initialized) return;
    if (_hsm->doReturnClauses) {
        // Cannot return right now: defer
        _temp_returned_clauses.push_back(clauses);
        return;
    }
    doReturnClauses(clauses);
}

void HordeProcessAdapter::doReturnClauses(const std::vector<int>& clauses) {
    _hsm->returnedBufferSize = clauses.size();
    memcpy(_returned_buffer, clauses.data(),
        std::min((size_t)_hsm->importBufferMaxSize, clauses.size()) * sizeof(int));
    _hsm->doReturnClauses = true;
    if (_hsm->isInitialized) Process::wakeUp(_child_pid);
}

void HordeProcessAdapter::dumpStats() {
    if (!_initialized) return;
    _hsm->doDumpStats = true;
    // No hard need to wake up immediately
}

HordeProcessAdapter::SubprocessStatus HordeProcessAdapter::check() {
    if (!_initialized) return NORMAL;

    int exitStatus;
    if (Process::didChildExit(_child_pid, &exitStatus) && exitStatus != 0) {
        // Child exited!
        if (exitStatus == SIGUSR2) {
            LOG(V3_VERB, "Restarting non-incremental child %ld\n", _child_pid);
        } else {
            LOG(V1_WARN, "[WARN] Child %ld exited unexpectedly (status %i)\n", _child_pid, exitStatus);
        }
        // Notify to restart solver engine
        return CRASHED;
    }

    doWriteRevisions();

    if (_hsm->didImport)            _hsm->doImport            = false;
    if (_hsm->didReturnClauses)     _hsm->doReturnClauses     = false;
    if (_hsm->didStartNextRevision) _hsm->doStartNextRevision = false;
    if (_hsm->didDumpStats)         _hsm->doDumpStats         = false;

    if (!_hsm->doStartNextRevision 
        && !_hsm->didStartNextRevision 
        && _published_revision < _written_revision) {
        _published_revision++;
        _hsm->desiredRevision = _desired_revision;
        _hsm->doStartNextRevision = true;
    }

    if (!_hsm->doImport && !_hsm->didImport && !_temp_clause_buffers.empty()) {
        // Digest next clause buffer from intermediate storage
        doDigest(_temp_clause_buffers.front().first, _temp_clause_buffers.front().second);
        _temp_clause_buffers.erase(_temp_clause_buffers.begin());
    }

    if (!_hsm->doReturnClauses && !_hsm->didReturnClauses && !_temp_returned_clauses.empty()) {
        doReturnClauses(_temp_returned_clauses.front());
        _temp_returned_clauses.erase(_temp_returned_clauses.begin());
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
            _solution_prepare_future = ProcessWideThreadPool::get().addTask([&]() {
                doPrepareSolution();
            });
        }
    } 
    return NORMAL;
}

void HordeProcessAdapter::doPrepareSolution() {

    int rev = _solution_revision_in_preparation;
    size_t* solutionSize = (size_t*) SharedMemory::access(_shmem_id + ".solutionsize." + std::to_string(rev), sizeof(size_t));
    if (*solutionSize == 0) {
        _solution.result = _hsm->result;
        _solution.setSolutionToSerialize(nullptr, 0);
        _solution_in_preparation = false;
        return;
    } 

    // ACCESS the existing shared memory segment to the solution vector
    int* shmemSolution = (int*) SharedMemory::access(_shmem_id + ".solution." + std::to_string(rev), *solutionSize*sizeof(int));
    
    _solution.result = _hsm->result;
    _solution.setSolutionToSerialize(shmemSolution, *solutionSize);
    _solution_in_preparation = false;
}

JobResult& HordeProcessAdapter::getSolution() {
    return _solution;
}

void HordeProcessAdapter::waitUntilChildExited() {
    if (!_running) return;
    while (true) {
        // Wait until initialized
        if (_initialized) {
            // Check if child exited
            auto lock = _state_mutex.getLock();
            if (_child_pid == -1 || Process::didChildExit(_child_pid)) 
                return;
        }
        usleep(100*1000); // 0.1s
    }
}

void* HordeProcessAdapter::createSharedMemoryBlock(std::string shmemSubId, size_t size, void* data) {
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

HordeProcessAdapter::~HordeProcessAdapter() {
    freeSharedMemory();
    if (_clause_comm != nullptr) delete _clause_comm;
}

void HordeProcessAdapter::freeSharedMemory() {
    
    if (!_terminate) {
        _terminate = true;

        // wait for termination of background threads
        if (_bg_initializer.valid()) _bg_initializer.get();
        if (_bg_writer.valid()) _bg_writer.get();
        if (_solution_prepare_future.valid()) _solution_prepare_future.get();
    }

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

    for (auto& shmemObj : _shmem) {
        //log(V4_VVER, "DBG deleting %s\n", shmemObj.id.c_str());
        SharedMemory::free(shmemObj.id, (char*)shmemObj.data, shmemObj.size);
    }
    _shmem.clear();
}
