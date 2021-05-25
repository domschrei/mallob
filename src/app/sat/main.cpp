
#include <iostream>
#include <set>
#include <stdlib.h>
#include <unistd.h>
#include <map>
#include <string>
#include <vector>
#include <memory>
#include <assert.h>

#include "util/sys/timer.hpp"
#include "util/logger.hpp"
#include "util/params.hpp"
#include "util/sys/shared_memory.hpp"
#include "util/sys/process.hpp"
#include "util/sys/proc.hpp"
#include "data/checksum.hpp"

#include "app/sat/horde_process_adapter.hpp"
#include "hordesat/horde.hpp"

#ifndef MALLOB_VERSION
#define MALLOB_VERSION "(dbg)"
#endif

Logger getLog(const Parameters& params) {
    return Logger::getMainInstance().copy("<" + params.getParam("jobstr") + ">", "#" + params.getParam("jobid") + ".");
}

void* accessMemory(const Logger& log, const std::string& shmemId, size_t size) {
    void* ptr = SharedMemory::access(shmemId, size);
    if (ptr == nullptr) {
        log.log(V0_CRIT, "Could not access shmem %s! Aborting.\n", shmemId.c_str());  
        Process::doExit(0);  
    }
    return ptr;
}

void updateChecksum(Checksum* checksum, int* ptr, size_t size) {
    if (checksum == nullptr) return;
    for (size_t i = 0; i < size; i++) checksum->combine(ptr[i]);
}

void importRevision(const Logger& log, HordeLib& hlib, const std::string& shmemId, int revision, Checksum* checksum) {
    size_t* fSizePtr = (size_t*) accessMemory(log, shmemId + ".fsize." + std::to_string(revision), sizeof(size_t));
    size_t* aSizePtr = (size_t*) accessMemory(log, shmemId + ".asize." + std::to_string(revision), sizeof(size_t));
    log.log(V4_VVER, "Loading rev. %i : %i lits, %i assumptions\n", revision, *fSizePtr, *aSizePtr);
    int* fPtr = (int*) accessMemory(log, shmemId + ".formulae." + std::to_string(revision), sizeof(int) * (*fSizePtr));
    int* aPtr = (int*) accessMemory(log, shmemId + ".assumptions." + std::to_string(revision), sizeof(int) * (*aSizePtr));
    
    if (checksum != nullptr) {
        // Append accessed data to local checksum
        updateChecksum(checksum, fPtr, *fSizePtr);
        // Access checksum from outside
        Checksum* chk = (Checksum*) accessMemory(log, shmemId + ".checksum." + std::to_string(revision), sizeof(Checksum));
        if (chk->count() > 0) {
            // Check checksum
            if (checksum->get() != chk->get()) {
                log.log(V0_CRIT, "ERROR: Checksum fail at rev. %i. Incoming count: %ld ; local count: %ld\n", revision, chk->count(), checksum->count());
                abort();
            }
        }
    }

    hlib.appendRevision(revision, *fSizePtr, fPtr, *aSizePtr, aPtr);
}

void runSolverEngine(const Logger& log, const Parameters& programParams) {
    
    // Set up "management" block of shared memory created by the parent
    std::string shmemId = "/edu.kit.iti.mallob." + std::to_string(Proc::getParentPid()) + "." + programParams.getParam("mpirank") + ".#" + programParams.getParam("jobid");
    log.log(V4_VVER, "Access base shmem: %s\n", shmemId.c_str());
    HordeSharedMemory* hsm = (HordeSharedMemory*) accessMemory(log, shmemId, sizeof(HordeSharedMemory));

    // Read formulae and assumptions from other individual blocks of shared memory

    // Set up export and import buffers for clause exchanges
    int maxExportBufferSize = programParams.getIntParam("cbbs") * sizeof(int);
    int* exportBuffer = (int*) accessMemory(log, shmemId + ".clauseexport", maxExportBufferSize);
    int maxImportBufferSize = programParams.getIntParam("cbbs") * sizeof(int) * programParams.getIntParam("mpisize");
    int* importBuffer = (int*) accessMemory(log, shmemId + ".clauseimport", maxImportBufferSize);

    // Signal initialization to parent
    hsm->isSpawned = true;
    
    Checksum* checksum = programParams.isNotNull("checksums") ? new Checksum() : nullptr;

    // Prepare solver
    HordeLib hlib(programParams, log.copy("H", "H"));
    // Import first revision
    {
        int* fPtr = (int*) accessMemory(log, shmemId + ".formulae.0", sizeof(int) * hsm->fSize);
        int* aPtr = (int*) accessMemory(log, shmemId + ".assumptions.0", sizeof(int) * hsm->aSize);
        hlib.appendRevision(0, hsm->fSize, fPtr, hsm->aSize, aPtr);
        updateChecksum(checksum, fPtr, hsm->fSize);
    }
    // Import subsequent revisions
    int lastImportedRevision = 0;
    while (!hsm->doStartNextRevision && lastImportedRevision < hsm->revision) {
        lastImportedRevision++;
        importRevision(log, hlib, shmemId, lastImportedRevision, checksum);
    }
    // Start solver threads
    hlib.solve();
    
    std::vector<int> solutionVec;
    std::string solutionShmemId = "";
    char* solutionShmem;
    int solutionShmemSize = 0;
    int lastSolvedRevision = -1;

    // Main loop
    while (true) {

        // Wait until something happens
        // (can be interrupted by Fork::wakeUp(hsm->childPid))
        float time = Timer::elapsedSeconds();
        int sleepStatus = usleep(1000 /*1 millisecond*/);
        time = Timer::elapsedSeconds() - time;
        if (sleepStatus != 0) log.log(V5_DEBG, "Woken up after %i us\n", (int) (1000*1000*time));

        // Terminate
        if (hsm->doTerminate) {
            log.log(V5_DEBG, "DO terminate\n");
            hlib.dumpStats(/*final=*/true);
            break;
        }

        // Uninterrupt solvers
        if (hsm->doStartNextRevision && !hsm->didStartNextRevision) {
            log.log(V5_DEBG, "DO start next revision\n");
            hlib.interrupt();
            // Read all revisions since last solve
            while (lastImportedRevision < hsm->revision) {
                lastImportedRevision++;
                importRevision(log, hlib, shmemId, lastImportedRevision, checksum);
            }
            hlib.solve();
            hsm->didStartNextRevision = true;
        }
        if (!hsm->doStartNextRevision) hsm->didStartNextRevision = false;

        // Dump stats
        if (hsm->doDumpStats && !hsm->didDumpStats) {
            log.log(V5_DEBG, "DO dump stats\n");
            
            hlib.dumpStats(/*final=*/false);

            // For this management thread
            double cpuShare; float sysShare;
            bool success = Proc::getThreadCpuRatio(Proc::getTid(), cpuShare, sysShare);
            if (success) {
                log.log(V2_INFO, "child_main cpuratio=%.3f sys=%.3f\n", cpuShare, sysShare);
            }

            // For each solver thread
            std::vector<long> threadTids = hlib.getSolverTids();
            for (size_t i = 0; i < threadTids.size(); i++) {
                if (threadTids[i] < 0) continue;
                
                success = Proc::getThreadCpuRatio(threadTids[i], cpuShare, sysShare);
                if (success) {
                    log.log(V2_INFO, "td.%ld cpuratio=%.3f sys=%.3f\n", threadTids[i], cpuShare, sysShare);
                }
            }

            hsm->didDumpStats = true;
        }
        if (!hsm->doDumpStats) hsm->didDumpStats = false;

        // Check if clauses should be exported
        if (hsm->doExport && !hsm->didExport) {
            log.log(V5_DEBG, "DO export clauses\n");
            // Collect local clauses, put into shared memory
            hsm->exportChecksum = Checksum();
            hsm->exportBufferTrueSize = hlib.prepareSharing(exportBuffer, hsm->exportBufferMaxSize, hsm->exportChecksum);
            hsm->didExport = true;
        }
        if (!hsm->doExport) hsm->didExport = false;

        // Check if clauses should be imported
        if (hsm->doImport && !hsm->didImport) {
            log.log(V5_DEBG, "DO import clauses\n");
            // Write imported clauses from shared memory into vector
            hlib.digestSharing(importBuffer, hsm->importBufferSize, hsm->importChecksum);
            hsm->didImport = true;
        }
        if (!hsm->doImport) hsm->didImport = false;

        // Check initialization state
        if (!hsm->isInitialized && hlib.isFullyInitialized()) {
            log.log(V5_DEBG, "DO set initialized\n");
            hsm->isInitialized = true;
        }

        // Do not check solved state if the current 
        // revision has already been solved
        if (lastSolvedRevision == lastImportedRevision) continue;

        // Check solved state
        int resultCode = hlib.solveLoop();
        if (resultCode >= 0) {
            log.log(V5_DEBG, "DO write solution\n");
            // Solution found!
            auto& result = hlib.getResult();
            result.id = programParams.getIntParam("jobid");
            assert(result.revision == lastImportedRevision);
            solutionVec = result.solution;
            hsm->solutionRevision = result.revision;
            hsm->result = SatResult(result.result);
            hsm->solutionSize = solutionVec.size();
            // Write solution
            if (hsm->solutionSize > 0) {
                solutionShmemId = shmemId + ".solution." + std::to_string(hsm->solutionRevision);
                solutionShmemSize =  hsm->solutionSize*sizeof(int);
                solutionShmem = (char*) SharedMemory::create(solutionShmemId, solutionShmemSize);
                memcpy(solutionShmem, solutionVec.data(), solutionShmemSize);
            }
            lastSolvedRevision = lastImportedRevision;
            log.log(V5_DEBG, "DONE write solution\n");
            hsm->hasSolution = true;
            continue;
        }
    }

    hsm->didTerminate = true;
    log.flush();
    
    // Exit normally, but avoid calling destructors (some threads may be unresponsive)
    Process::doExit(0);

    // Shared memory will be cleaned up by the parent process.
}

int main(int argc, char *argv[]) {
    
    Parameters params;
    params.init(argc, argv);
    
    Timer::init(params.getDoubleParam("starttime"));

    int rankOfParent = params.getIntParam("mpirank");

    Random::init(params.getIntParam("mpisize"), rankOfParent);

    // Initialize signal handlers
    Process::init(rankOfParent, /*leafProcess=*/true);

    std::string logdir = params.getParam("log");
    Logger::init(rankOfParent, params.getIntParam("v"), params.isNotNull("colors"), 
            /*quiet=*/params.isNotNull("q"), /*cPrefix=*/params.isNotNull("mono"),
            params.isNotNull("nolog") ? nullptr : &logdir);
    
    auto log = getLog(params);
    pid_t pid = Proc::getPid();
    log.log(V3_VERB, "mallob SAT engine %s pid=%lu\n", MALLOB_VERSION, pid);

    try {
        // Launch program
        runSolverEngine(log, params);

    } catch (const std::exception &ex) {
        log.log(V0_CRIT, "Unexpected ERROR: \"%s\" - aborting\n", ex.what());
        log.flush();
        Process::doExit(1);
    } catch (...) {
        log.log(V0_CRIT, "Unexpected ERROR - aborting\n");
        log.flush();
        Process::doExit(1);
    }
}
