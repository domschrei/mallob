
#include <assert.h>
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

HordeProcessAdapter::HordeProcessAdapter(const Parameters& params, HordeConfig&& config,
    size_t fSize, const int* fLits, size_t aSize, const int* aLits) :    
        _params(params), _f_size(fSize), _f_lits(fLits), _a_size(aSize), _a_lits(aLits) {

    initSharedMemory(std::move(config));
}

void HordeProcessAdapter::initSharedMemory(HordeConfig&& config) {

    // Initialize "management" shared memory
    _shmem_id = "/edu.kit.iti.mallob." + std::to_string(Proc::getPid()) + "." + std::to_string(config.mpirank) + ".#" + std::to_string(config.jobid);
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
    _hsm->firstRevision = config.firstrev;
    _hsm->revision = 0;

    _export_buffer = (int*) createSharedMemoryBlock("clauseexport", 
            _params.clauseBufferBaseSize() * sizeof(int), nullptr);
    _hsm->importBufferMaxSize = _params.clauseHistoryAggregationFactor() * MyMpi::getBinaryTreeBufferLimit(
        config.mpisize, _params.clauseBufferBaseSize(), _params.clauseBufferDiscountFactor(), 
        MyMpi::ALL
    ) + 1024;
    _import_buffer = (int*) createSharedMemoryBlock("clauseimport", 
            sizeof(int)*_hsm->importBufferMaxSize, nullptr);
    _hsm->config = std::move(config);
    _latest_published_revision = 0;
    
    _concurrent_shmem_allocator = std::thread([this]() {

        createSharedMemoryBlock("formulae.0", sizeof(int) * _f_size, (void*)_f_lits);
        createSharedMemoryBlock("assumptions.0", sizeof(int) * _a_size, (void*)_a_lits);
        _latest_ready_revision = 0;

        _hsm->doBegin = true;

        while (!_do_terminate) {
            usleep(1000*10);
            RevisionData revData;
            {
                auto lock = _revisions_mutex.getLock();
                if (_revisions_to_write.empty()) continue;
                revData = _revisions_to_write.front();
                _revisions_to_write.erase(_revisions_to_write.begin());
            }
            auto revStr = std::to_string(revData.revision);
            createSharedMemoryBlock("fsize."       + revStr, sizeof(size_t),              (void*)&revData.fSize);
            createSharedMemoryBlock("asize."       + revStr, sizeof(size_t),              (void*)&revData.aSize);
            createSharedMemoryBlock("formulae."    + revStr, sizeof(int) * revData.fSize, (void*)revData.fLits);
            createSharedMemoryBlock("assumptions." + revStr, sizeof(int) * revData.aSize, (void*)revData.aLits);
            createSharedMemoryBlock("checksum."    + revStr, sizeof(Checksum),            (void*)&(revData.checksum));
            log(V4_VVER, "Revision %i ready!\n", revData.revision);
            _latest_ready_revision = revData.revision;
        }
    });
}

HordeProcessAdapter::~HordeProcessAdapter() {
    freeSharedMemory();
}

void HordeProcessAdapter::freeSharedMemory() {
    if (_hsm != nullptr) {
        
        _do_terminate = true;
        if (_concurrent_shmem_allocator.joinable())
            _concurrent_shmem_allocator.join();
        
        for (int rev = 0; rev <= _hsm->revision; rev++) {
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

pid_t HordeProcessAdapter::run() {

    // Assemble c-style program arguments
    const char* execName = "mallob_sat_process";
    char* const* argv = _params.asCArgs(execName);
    
    // FORK: Create a child process
    pid_t res = Process::createChild();
    if (res > 0) {
        // [parent process]
        _child_pid = res;
        //while (!_hsm->isSpawned) usleep(250 /* 1/4 milliseconds */);
        _state = SolvingStates::ACTIVE;

        int i = 1;
        while (argv[i] != nullptr) free(argv[i++]);
        delete[] argv;

        
        return res;
    }

    // [child process]
    // Execute the SAT process.
    int result = execvp("mallob_sat_process", argv);
    
    // If this is reached, something went wrong with execvp
    log(V0_CRIT, "ERROR: execvp returned %i with errno %i\n", result, (int)errno);
    abort();
}

bool HordeProcessAdapter::isFullyInitialized() {
    return _hsm->isInitialized;
}

pid_t HordeProcessAdapter::getPid() {
    return _child_pid;
}

void HordeProcessAdapter::appendRevisions(const std::vector<RevisionData>& revisions) {
    auto lock = _revisions_mutex.getLock();
    _revisions_to_write.insert(_revisions_to_write.end(), revisions.begin(), revisions.end());
}

void HordeProcessAdapter::setSolvingState(SolvingStates::SolvingState state) {
    if (state == _state) return;

    if (state == SolvingStates::ABORTING) {
        //Fork::terminate(_child_pid); // Terminate child process by signal.
        _hsm->doTerminate = true; // Kindly ask child process to terminate.
        Process::resume(_child_pid); // Continue (resume) process.
    }
    if (state == SolvingStates::SUSPENDED) {
        Process::suspend(_child_pid); // Stop (suspend) process.
    }
    if (state == SolvingStates::ACTIVE) {
        _hsm->hasSolution = false;
        Process::resume(_child_pid); // Continue (resume) process.
    }

    _state = state;
}

void HordeProcessAdapter::collectClauses(int maxSize) {
    _hsm->exportBufferMaxSize = maxSize;
    _hsm->doExport = true;
    if (_hsm->isInitialized) Process::wakeUp(_child_pid);
}
bool HordeProcessAdapter::hasCollectedClauses() {
    return _hsm->didExport;
}
std::vector<int> HordeProcessAdapter::getCollectedClauses(Checksum& checksum) {
    if (!_hsm->didExport) {
        return std::vector<int>();
    }
    std::vector<int> clauses(_hsm->exportBufferTrueSize);
    memcpy(clauses.data(), _export_buffer, clauses.size()*sizeof(int));
    checksum = _hsm->exportChecksum;
    _hsm->doExport = false;
    return clauses;
}

void HordeProcessAdapter::digestClauses(const std::vector<int>& clauses, const Checksum& checksum) {
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
    assert(_hsm->importBufferSize <= _hsm->importBufferMaxSize);
    memcpy(_import_buffer, clauses.data(), clauses.size()*sizeof(int));
    _hsm->doImport = true;
    if (_hsm->isInitialized) Process::wakeUp(_child_pid);
}

void HordeProcessAdapter::dumpStats() {
    _hsm->doDumpStats = true;
    // No hard need to wake up immediately
}

bool HordeProcessAdapter::check() {

    if (_hsm->didImport)            _hsm->doImport            = false;
    if (_hsm->didStartNextRevision) _hsm->doStartNextRevision = false;
    if (_hsm->didDumpStats)         _hsm->doDumpStats         = false;

    if (!_hsm->doStartNextRevision 
        && !_hsm->didStartNextRevision 
        && _latest_published_revision < _latest_ready_revision) {
        
        _latest_published_revision++;
        _hsm->revision = _latest_published_revision;
        _hsm->doStartNextRevision = true;
    }

    if (!_hsm->doImport && !_hsm->didImport && !_temp_clause_buffers.empty()) {
        // Digest next clause buffer from intermediate storage
        doDigest(_temp_clause_buffers.front().first, _temp_clause_buffers.front().second);
        _temp_clause_buffers.erase(_temp_clause_buffers.begin());
    }
    
    if (_hsm->hasSolution) return _hsm->solutionRevision == _hsm->revision;
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

