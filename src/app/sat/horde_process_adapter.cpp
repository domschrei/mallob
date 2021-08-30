
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
    size_t fSize, const int* fLits, size_t aSize, const int* aLits) :    
        _params(std::move(params)), _config(std::move(config)), _job(job),
        _f_size(fSize), _f_lits(fLits), _a_size(aSize), _a_lits(aLits) {

    _written_revision = -1;
    _desired_revision = _config.firstrev;
}

void HordeProcessAdapter::run() {
    _running = true;
    ProcessWideThreadPool::get().addTask(
        std::bind(&HordeProcessAdapter::doInitialize, this)
    );
}

void HordeProcessAdapter::doInitialize() {

    _clause_comm = new AnytimeSatClauseCommunicator(_params, _job);

    // Initialize "management" shared memory
    _shmem_id = "/edu.kit.iti.mallob." + std::to_string(Proc::getPid()) + "." 
        + std::to_string(_config.mpirank) + ".#" + std::to_string(_config.jobid);
    //log(V4_VVER, "Setup base shmem: %s\n", _shmem_id.c_str());
    void* mainShmem = SharedMemory::create(_shmem_id, sizeof(HordeSharedMemory));
    _shmem.insert(ShmemObject{_shmem_id, mainShmem, sizeof(HordeSharedMemory)});
    _hsm = new ((char*)mainShmem) HordeSharedMemory();
    _hsm->doBegin = false;
    _hsm->doExport = false;
    _hsm->doImport = false;
    _hsm->doDumpStats = false;
    _hsm->doStartNextRevision = false;
    _hsm->doTerminate = false;
    _hsm->exportBufferMaxSize = 0;
    _hsm->importBufferSize = 0;
    _hsm->didExport = false;
    _hsm->didImport = false;
    _hsm->didDumpStats = false;
    _hsm->didStartNextRevision = false;
    _hsm->didTerminate = false;
    _hsm->isSpawned = false;
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

    // Allocate shared memory for formula, assumptions of initial revision
    createSharedMemoryBlock("formulae.0", sizeof(int) * _f_size, (void*)_f_lits);
    createSharedMemoryBlock("assumptions.0", sizeof(int) * _a_size, (void*)_a_lits);
    _written_revision = 0;

    // Assemble c-style program arguments
    const char* execName = "mallob_sat_process";
    char* const* argv = _params.asCArgs(execName);
    
    // FORK: Create a child process
    pid_t res = Process::createChild();
    if (res == 0) {
        // [child process]
        // Execute the SAT process.
        int result = execvp("mallob_sat_process", argv);
        
        // If this is reached, something went wrong with execvp
        log(V0_CRIT, "[ERROR] execvp returned %i with errno %i\n", result, (int)errno);
        abort();
    }

    // [parent process]
    int i = 1;
    while (argv[i] != nullptr) free(argv[i++]);
    delete[] argv;

    _initialized = true;
    _hsm->doBegin = true;

    {
        auto lock = _state_mutex.getLock();
        _child_pid = res;
        applySolvingState();
    }
}

void HordeProcessAdapter::startBackgroundWriterIfNecessary() {
    if (!_initialized || _num_revisions_to_write == 0) return;
    {
        auto lock = _state_mutex.getLock();
        if (!_bg_writer_running) {
            _bg_writer_running = true;
            _bg_writer = ProcessWideThreadPool::get().addTask(
                std::bind(&HordeProcessAdapter::doWriteNextRevision, this)
            );
        }
    }
}

void HordeProcessAdapter::doWriteNextRevision() {
    
    while (true) {
        RevisionData revData;
        {
            auto lock = _revisions_mutex.getLock();
            if (_revisions_to_write.empty()) break;
            revData = _revisions_to_write.front();
            _revisions_to_write.erase(_revisions_to_write.begin());
            _num_revisions_to_write--;
        }
        auto revStr = std::to_string(revData.revision);
        createSharedMemoryBlock("fsize."       + revStr, sizeof(size_t),              (void*)&revData.fSize);
        createSharedMemoryBlock("asize."       + revStr, sizeof(size_t),              (void*)&revData.aSize);
        createSharedMemoryBlock("formulae."    + revStr, sizeof(int) * revData.fSize, (void*)revData.fLits);
        createSharedMemoryBlock("assumptions." + revStr, sizeof(int) * revData.aSize, (void*)revData.aLits);
        createSharedMemoryBlock("checksum."    + revStr, sizeof(Checksum),            (void*)&(revData.checksum));
        _written_revision = revData.revision;
    }

    auto lock = _state_mutex.getLock();
    _bg_writer_running = false;
}

bool HordeProcessAdapter::hasClauseComm() {
    return _initialized;
}

bool HordeProcessAdapter::isFullyInitialized() {
    return _initialized && _hsm->isInitialized;
}

void HordeProcessAdapter::appendRevisions(const std::vector<RevisionData>& revisions, int desiredRevision) {
    auto lock = _revisions_mutex.getLock();
    _revisions_to_write.insert(_revisions_to_write.end(), revisions.begin(), revisions.end());
    _desired_revision = desiredRevision;
    _num_revisions_to_write++;
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
    std::vector<int> clauses(_hsm->exportBufferTrueSize);
    memcpy(clauses.data(), _export_buffer, clauses.size()*sizeof(int));
    checksum = _hsm->exportChecksum;
    _hsm->doExport = false;
    return clauses;
}

void HordeProcessAdapter::digestClauses(const std::vector<int>& clauses, const Checksum& checksum) {
    if (!_initialized) return;
    if (_hsm->doImport) {
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

void HordeProcessAdapter::dumpStats() {
    if (!_initialized) return;
    _hsm->doDumpStats = true;
    // No hard need to wake up immediately
}

bool HordeProcessAdapter::check() {
    if (!_initialized) return false;

    startBackgroundWriterIfNecessary();

    if (_hsm->didImport)            _hsm->doImport            = false;
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
    
    if (_hsm->hasSolution) return _hsm->solutionRevision == _desired_revision;
    return false;
}

std::pair<SatResult, std::vector<int>> HordeProcessAdapter::getSolution() {

    int rev = _hsm->solutionRevision;
    size_t* solutionSize = (size_t*) SharedMemory::access(_shmem_id + ".solutionsize." + std::to_string(rev), sizeof(size_t));
    if (*solutionSize == 0) return std::pair<SatResult, std::vector<int>>(_hsm->result, std::vector<int>()); 

    std::vector<int> solution(*solutionSize);

    // ACCESS the existing shared memory segment to the solution vector
    int* shmemSolution = (int*) SharedMemory::access(_shmem_id + ".solution." + std::to_string(rev), solution.size()*sizeof(int));
    memcpy(solution.data(), shmemSolution, solution.size()*sizeof(int));
    
    return std::pair<SatResult, std::vector<int>>(_hsm->result, solution);
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
    return shmem;
}

HordeProcessAdapter::~HordeProcessAdapter() {
    freeSharedMemory();
    if (_clause_comm != nullptr) delete _clause_comm;
}

void HordeProcessAdapter::freeSharedMemory() {
    _terminate = true;
    if (_bg_writer_running) _bg_writer.get(); // wait for termination of background writer
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
        SharedMemory::free(shmemObj.id, (char*)shmemObj.data, shmemObj.size);
    }
    _shmem.clear();
}
