
#pragma once

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
#include "util/sys/terminator.hpp"

#include "engine.hpp"
#include "../job/sat_shared_memory.hpp"

class SatProcess {

private:
    const Parameters& _params;
    const SatProcessConfig& _config;
    Logger& _log;
    SatEngine _engine;

    std::string _shmem_id;
    SatSharedMemory* _hsm;
    int* _export_buffer;
    int* _import_buffer;
    int* _filter_buffer;
    int* _returned_buffer;

    int _last_imported_revision;
    int _desired_revision;
    Checksum* _checksum;

public:
    SatProcess(const Parameters& params, const SatProcessConfig& config, Logger& log) 
        : _params(params), _config(config), _log(log), _engine(_params, _config, _log) {

        // Set up "management" block of shared memory created by the parent
        _shmem_id = _config.getSharedMemId(Proc::getParentPid());
        LOGGER(log, V4_VVER, "Access base shmem: %s\n", _shmem_id.c_str());
        _hsm = (SatSharedMemory*) accessMemory(_shmem_id, sizeof(SatSharedMemory));
        
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
            int maxExportBufferSize = _hsm->exportBufferAllocatedSize * sizeof(int);
            _export_buffer = (int*) accessMemory(_shmem_id + ".clauseexport", maxExportBufferSize);
            int maxImportBufferSize = _hsm->importBufferMaxSize * sizeof(int);
            _import_buffer = (int*) accessMemory(_shmem_id + ".clauseimport", maxImportBufferSize);
            int maxFilterSize = _hsm->importBufferMaxSize/8 + 1;
            _filter_buffer = (int*) accessMemory(_shmem_id + ".clausefilter", maxFilterSize);
            _returned_buffer = (int*) accessMemory(_shmem_id + ".returnedclauses", maxImportBufferSize);
        }

        // Import first revision
        _desired_revision = _config.firstrev;
        {
            int* fPtr = (int*) accessMemory(_shmem_id + ".formulae.0", sizeof(int) * _hsm->fSize);
            int* aPtr = (int*) accessMemory(_shmem_id + ".assumptions.0", sizeof(int) * _hsm->aSize);
            _engine.appendRevision(0, _hsm->fSize, fPtr, _hsm->aSize, aPtr, 
                /*finalRevisionForNow=*/_desired_revision == 0);
            updateChecksum(fPtr, _hsm->fSize);
        }
        _last_imported_revision = 0;
        // Import subsequent revisions
        importRevisions();
        
        // Start solver threads
        _engine.solve();
        
        std::vector<int> solutionVec;
        std::string solutionShmemId = "";
        char* solutionShmem;
        int solutionShmemSize = 0;
        int lastSolvedRevision = -1;

        // Main loop
        while (true) {

            doSleep();
            Timer::cacheElapsedSeconds();

            // Terminate
            if (_hsm->doTerminate || Terminator::isTerminating(/*fromMainThread=*/true)) {
                LOGGER(_log, V5_DEBG, "DO terminate\n");
                _engine.dumpStats(/*final=*/true);
                break;
            }

            // Read new revisions as necessary
            importRevisions();

            // Dump stats
            if (_hsm->doDumpStats && !_hsm->didDumpStats) {
                LOGGER(_log, V5_DEBG, "DO dump stats\n");
                
                _engine.dumpStats(/*final=*/false);

                // For this management thread
                double cpuShare; float sysShare;
                bool success = Proc::getThreadCpuRatio(Proc::getTid(), cpuShare, sysShare);
                if (success) {
                    LOGGER(_log, V3_VERB, "child_main cpuratio=%.3f sys=%.3f\n", cpuShare, sysShare);
                }

                // For each solver thread
                std::vector<long> threadTids = _engine.getSolverTids();
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
                _hsm->exportBufferTrueSize = _engine.prepareSharing(_export_buffer, _hsm->exportBufferMaxSize);
                auto [admitted, total] = _engine.getLastAdmittedClauseShare();
                _hsm->lastNumAdmittedClausesToImport = admitted;
                _hsm->lastNumClausesToImport = total;
                assert(_hsm->exportBufferTrueSize <= _hsm->exportBufferAllocatedSize);
                _hsm->didExport = true;
            }
            if (!_hsm->doExport) _hsm->didExport = false;

            // Check if clauses should be filtered
            if (_hsm->doFilterImport && !_hsm->didFilterImport) {
                LOGGER(_log, V5_DEBG, "DO filter clauses\n");
                _hsm->filterSize = _engine.filterSharing(_import_buffer, _hsm->importBufferSize, _filter_buffer);
                _hsm->didFilterImport = true;
            }
            if (!_hsm->doFilterImport) _hsm->didFilterImport = false;

            // Check if clauses should be digested (must not be "from the future")
            if ((_hsm->doDigestImportWithFilter || _hsm->doDigestImportWithoutFilter) 
                    && !_hsm->didDigestImport && _hsm->importBufferRevision <= _last_imported_revision) {
                LOGGER(_log, V5_DEBG, "DO import clauses\n");
                // Write imported clauses from shared memory into vector
                assert(_hsm->importBufferSize <= _hsm->importBufferMaxSize);
                if (_hsm->doDigestImportWithFilter) {
                    _engine.digestSharingWithFilter(_import_buffer, _hsm->importBufferSize, _filter_buffer);
                } else {
                    _engine.digestSharingWithoutFilter(_import_buffer, _hsm->importBufferSize);
                }
                _hsm->didDigestImport = true;
            }
            if (!_hsm->doDigestImportWithFilter && !_hsm->doDigestImportWithoutFilter) 
                _hsm->didDigestImport = false;

            // Re-insert returned clauses into the local clause database to be exported later
            if (_hsm->doReturnClauses && !_hsm->didReturnClauses) {
                LOGGER(_log, V5_DEBG, "DO return clauses\n");
                _engine.returnClauses(_returned_buffer, _hsm->returnedBufferSize);
                _hsm->didReturnClauses = true;
            }
            if (!_hsm->doReturnClauses) _hsm->didReturnClauses = false;

            // Check initialization state
            if (!_hsm->isInitialized && _engine.isFullyInitialized()) {
                LOGGER(_log, V5_DEBG, "DO set initialized\n");
                _hsm->isInitialized = true;
            }
            
            // Terminate "improperly" in order to be restarted automatically
            if (_hsm->doCrash) {
                LOGGER(_log, V3_VERB, "Restarting this subprocess\n");
                raise(SIGUSR2);
            }

            // Do not check solved state if the current 
            // revision has already been solved
            if (lastSolvedRevision == _last_imported_revision) continue;

            // Check solved state
            int resultCode = _engine.solveLoop();
            if (resultCode >= 0) {
                // Solution found!
                auto& result = _engine.getResult();
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

        _engine.appendRevision(revision, *fSizePtr, fPtr, *aSizePtr, aPtr, 
            /*finalRevisionForNow=*/revision == _desired_revision);
    }

    void doSleep() {
        // Wait until something happens
        // (can be interrupted by Fork::wakeUp(hsm->childPid))
        usleep(1000 /*1 millisecond*/);
    }

    void doTerminate() {
        _hsm->didTerminate = true;
        _log.flush();   
        Process::doExit(0);
    }
};
