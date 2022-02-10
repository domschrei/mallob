
#ifndef DOMPASCH_MALLOB_HORDE_PROCESS_HPP
#define DOMPASCH_MALLOB_HORDE_PROCESS_HPP

#include <iostream>
#include <stdlib.h>
#include <unistd.h>
#include <string>
#include <vector>
#include <memory>
#include "util/assert.hpp"

#include "util/sys/timer.hpp"
#include "util/logger.hpp"
#include "util/params.hpp"
#include "util/sys/shared_memory.hpp"
#include "util/sys/process.hpp"
#include "util/sys/proc.hpp"
#include "data/checksum.hpp"

#include "app/sat/horde_process_adapter.hpp"
#include "hordesat/horde.hpp"

class HordeProcess {

private:
    const Parameters& _params;
    const HordeConfig& _config;
    Logger& _log;
    HordeLib _hlib;

    std::string _shmem_id;
    HordeSharedMemory* _hsm;
    int* _export_buffer;
    int* _import_buffer;
    int* _returned_buffer;

    int _last_imported_revision;
    int _desired_revision;
    Checksum* _checksum;

public:
    HordeProcess(const Parameters& params, const HordeConfig& config, Logger& log) 
        : _params(params), _config(config), _log(log), _hlib(_params, _config, _log) {

        // Set up "management" block of shared memory created by the parent
        _shmem_id = _config.getSharedMemId(Proc::getParentPid());
        LOGGER(log, V4_VVER, "Access base shmem: %s\n", _shmem_id.c_str());
        _hsm = (HordeSharedMemory*) accessMemory(_shmem_id, sizeof(HordeSharedMemory));
        
        _checksum = params.useChecksums() ? new Checksum() : nullptr;
    }

    void run() {
        // Wait until everything is prepared for the solver to begin
        while (!_hsm->doBegin) doSleep();
        
        // Terminate directly?
        if (_hsm->doTerminate || Terminator::isTerminating(/*fromMainThread=*/true)) 
            doTerminate();

        // Set up export and import buffers for clause exchanges
        {
            int maxExportBufferSize = _params.clauseBufferBaseSize() * sizeof(int);
            _export_buffer = (int*) accessMemory(_shmem_id + ".clauseexport", maxExportBufferSize);
            int maxImportBufferSize = _hsm->importBufferMaxSize * sizeof(int);
            _import_buffer = (int*) accessMemory(_shmem_id + ".clauseimport", maxImportBufferSize);
            _returned_buffer = (int*) accessMemory(_shmem_id + ".returnedclauses", maxImportBufferSize);
        }

        // Import first revision
        _last_imported_revision = 0;
        _desired_revision = _config.firstrev;
        {
            int* fPtr = (int*) accessMemory(_shmem_id + ".formulae.0", sizeof(int) * _hsm->fSize);
            int* aPtr = (int*) accessMemory(_shmem_id + ".assumptions.0", sizeof(int) * _hsm->aSize);
            _hlib.appendRevision(0, _hsm->fSize, fPtr, _hsm->aSize, aPtr, 
                /*finalRevisionForNow=*/_desired_revision == 0);
            updateChecksum(fPtr, _hsm->fSize);
        }
        // Import subsequent revisions
        importRevisions();
        
        // Start solver threads
        _hlib.solve();
        
        std::vector<int> solutionVec;
        std::string solutionShmemId = "";
        char* solutionShmem;
        int solutionShmemSize = 0;
        int lastSolvedRevision = -1;

        // Main loop
        while (true) {

            doSleep();

            // Terminate
            if (_hsm->doTerminate || Terminator::isTerminating(/*fromMainThread=*/true)) {
                LOGGER(_log, V5_DEBG, "DO terminate\n");
                _hlib.dumpStats(/*final=*/true);
                break;
            }

            // Read new revisions as necessary
            importRevisions();

            // Dump stats
            if (_hsm->doDumpStats && !_hsm->didDumpStats) {
                LOGGER(_log, V5_DEBG, "DO dump stats\n");
                
                _hlib.dumpStats(/*final=*/false);

                // For this management thread
                double cpuShare; float sysShare;
                bool success = Proc::getThreadCpuRatio(Proc::getTid(), cpuShare, sysShare);
                if (success) {
                    LOGGER(_log, V3_VERB, "child_main cpuratio=%.3f sys=%.3f\n", cpuShare, sysShare);
                }

                // For each solver thread
                std::vector<long> threadTids = _hlib.getSolverTids();
                for (size_t i = 0; i < threadTids.size(); i++) {
                    if (threadTids[i] < 0) continue;
                    
                    success = Proc::getThreadCpuRatio(threadTids[i], cpuShare, sysShare);
                    if (success) {
                        LOGGER(_log, V3_VERB, "td.%ld cpuratio=%.3f sys=%.3f\n", threadTids[i], cpuShare, sysShare);
                    }
                }

                auto rtInfo = Proc::getRuntimeInfo(Proc::getPid(), Proc::SubprocessMode::FLAT);
                LOGGER(_log, V3_VERB, "child_mem=%.3fGB\n", 0.001*0.001*rtInfo.residentSetSize);

                _hsm->didDumpStats = true;
            }
            if (!_hsm->doDumpStats) _hsm->didDumpStats = false;

            // Check if clauses should be exported
            if (_hsm->doExport && !_hsm->didExport) {
                LOGGER(_log, V5_DEBG, "DO export clauses\n");
                // Collect local clauses, put into shared memory
                _hsm->exportChecksum = Checksum();
                _hsm->exportBufferTrueSize = _hlib.prepareSharing(_export_buffer, _hsm->exportBufferMaxSize, _hsm->exportChecksum);
                _hsm->didExport = true;
            }
            if (!_hsm->doExport) _hsm->didExport = false;

            // Check if clauses should be imported (must not be "from the future")
            if (_hsm->doImport && !_hsm->didImport && _hsm->importBufferRevision <= _last_imported_revision) {
                LOGGER(_log, V5_DEBG, "DO import clauses\n");
                // Write imported clauses from shared memory into vector
                assert(_hsm->importBufferSize <= _hsm->importBufferMaxSize);
                _hlib.digestSharing(_import_buffer, _hsm->importBufferSize, _hsm->importChecksum);
                _hsm->didImport = true;
            }
            if (!_hsm->doImport) _hsm->didImport = false;

            // Re-insert returned clauses into the local clause database to be exported later
            if (_hsm->doReturnClauses && !_hsm->didReturnClauses) {
                LOGGER(_log, V5_DEBG, "DO return clauses\n");
                _hlib.returnClauses(_returned_buffer, _hsm->returnedBufferSize);
                _hsm->didReturnClauses = true;
            }
            if (!_hsm->doReturnClauses) _hsm->didReturnClauses = false;

            // Check initialization state
            if (!_hsm->isInitialized && _hlib.isFullyInitialized()) {
                LOGGER(_log, V5_DEBG, "DO set initialized\n");
                _hsm->isInitialized = true;
            }

            // Do not check solved state if the current 
            // revision has already been solved
            if (lastSolvedRevision == _last_imported_revision) continue;

            // Check solved state
            int resultCode = _hlib.solveLoop();
            if (resultCode >= 0) {
                // Solution found!
                auto& result = _hlib.getResult();
                result.id = _config.jobid;
                if (_hsm->doTerminate || result.revision < _desired_revision) {
                    // Result obsolete
                    continue;
                }
                assert(result.revision == _last_imported_revision);

                solutionVec = result.extractSolution();
                _hsm->solutionRevision = result.revision;
                LOGGER(_log, V5_DEBG, "DO write solution\n");
                _hsm->result = SatResult(result.result);
                size_t* solutionSize = (size_t*) SharedMemory::create(_shmem_id + ".solutionsize." + std::to_string(_hsm->solutionRevision), sizeof(size_t));
                *solutionSize = solutionVec.size();
                // Write solution
                if (*solutionSize > 0) {
                    solutionShmemId = _shmem_id + ".solution." + std::to_string(_hsm->solutionRevision);
                    solutionShmemSize =  *solutionSize*sizeof(int);
                    solutionShmem = (char*) SharedMemory::create(solutionShmemId, solutionShmemSize);
                    memcpy(solutionShmem, solutionVec.data(), solutionShmemSize);
                }
                lastSolvedRevision = result.revision;
                LOGGER(_log, V5_DEBG, "DONE write solution\n");
                _hsm->hasSolution = true;
            }
        }

        doTerminate();

        // Shared memory will be cleaned up by the parent process.
    }
    
private:
    void* accessMemory(const std::string& shmemId, size_t size) {
        void* ptr = SharedMemory::access(shmemId, size);
        if (ptr == nullptr) {
            LOGGER(_log, V0_CRIT, "[ERROR] Could not access shmem %s\n", shmemId.c_str());  
            Process::doExit(0);  
        }
        return ptr;
    }

    void updateChecksum(int* ptr, size_t size) {
        if (_checksum == nullptr) return;
        for (size_t i = 0; i < size; i++) _checksum->combine(ptr[i]);
    }

    void importRevisions() {
        while ((_hsm->doStartNextRevision && !_hsm->didStartNextRevision) 
                || _last_imported_revision < _desired_revision) {
            if (_hsm->doTerminate) doTerminate();
            if (_hsm->doStartNextRevision && !_hsm->didStartNextRevision) {
                _desired_revision = _hsm->desiredRevision;
                _last_imported_revision++;
                importRevision(_last_imported_revision, _checksum);
                _hsm->didStartNextRevision = true;
                _hsm->hasSolution = false;
            } else doSleep();
            if (!_hsm->doStartNextRevision) _hsm->didStartNextRevision = false;
        }
        if (!_hsm->doStartNextRevision) _hsm->didStartNextRevision = false;
    }

    void importRevision(int revision, Checksum* checksum) {
        size_t* fSizePtr = (size_t*) accessMemory(_shmem_id + ".fsize." + std::to_string(revision), sizeof(size_t));
        size_t* aSizePtr = (size_t*) accessMemory(_shmem_id + ".asize." + std::to_string(revision), sizeof(size_t));
        LOGGER(_log, V4_VVER, "Read rev. %i/%i : %i lits, %i assumptions\n", revision, _desired_revision, *fSizePtr, *aSizePtr);
        int* fPtr = (int*) accessMemory(_shmem_id + ".formulae." + std::to_string(revision), sizeof(int) * (*fSizePtr));
        int* aPtr = (int*) accessMemory(_shmem_id + ".assumptions." + std::to_string(revision), sizeof(int) * (*aSizePtr));
        
        if (checksum != nullptr) {
            // Append accessed data to local checksum
            updateChecksum(fPtr, *fSizePtr);
            // Access checksum from outside
            Checksum* chk = (Checksum*) accessMemory(_shmem_id + ".checksum." + std::to_string(revision), sizeof(Checksum));
            if (chk->count() > 0) {
                // Check checksum
                if (checksum->get() != chk->get()) {
                    LOGGER(_log, V0_CRIT, "[ERROR] Checksum fail at rev. %i. Incoming count: %ld ; local count: %ld\n", revision, chk->count(), checksum->count());
                    abort();
                }
            }
        }

        _hlib.appendRevision(revision, *fSizePtr, fPtr, *aSizePtr, aPtr, /*finalRevisionForNow=*/revision == _desired_revision);
    }

    void doSleep() {
        // Wait until something happens
        // (can be interrupted by Fork::wakeUp(hsm->childPid))
        float time = Timer::elapsedSeconds();
        int sleepStatus = usleep(1000 /*1 millisecond*/);
        time = Timer::elapsedSeconds() - time;
        if (sleepStatus != 0) LOGGER(_log, V5_DEBG, "Woken up after %i us\n", (int) (1000*1000*time));
    }

    void doTerminate() {
        _hsm->didTerminate = true;
        _log.flush();   
        Process::doExit(0);
    }
};

#endif
