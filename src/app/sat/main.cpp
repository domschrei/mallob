
#include <iostream>
#include <stdlib.h>
#include <unistd.h>
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

Logger getLog(const Parameters& params, const HordeConfig& config) {
    return Logger::getMainInstance().copy("<" + config.getJobStr() + ">", "#" + std::to_string(config.jobid) + ".");
}

void* accessMemory(const Logger& log, const std::string& shmemId, size_t size) {
    void* ptr = SharedMemory::access(shmemId, size);
    if (ptr == nullptr) {
        log.log(V0_CRIT, "ERROR: Could not access shmem %s! Aborting.\n", shmemId.c_str());  
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

void doSleep(const Logger& log) {
    // Wait until something happens
    // (can be interrupted by Fork::wakeUp(hsm->childPid))
    float time = Timer::elapsedSeconds();
    int sleepStatus = usleep(1000 /*1 millisecond*/);
    time = Timer::elapsedSeconds() - time;
    if (sleepStatus != 0) log.log(V5_DEBG, "Woken up after %i us\n", (int) (1000*1000*time));
}

void doTerminate(const Logger& log, HordeSharedMemory* hsm) {
    hsm->didTerminate = true;
    log.flush();   
    Process::doExit(0);
}

void runSolverEngine(const Logger& log, const Parameters& programParams, HordeConfig& config) {
    
    // Set up "management" block of shared memory created by the parent
    std::string shmemId = "/edu.kit.iti.mallob." + std::to_string(Proc::getParentPid()) + "." + std::to_string(config.mpirank) + ".#" + std::to_string(config.jobid);
    log.log(V4_VVER, "Access base shmem: %s\n", shmemId.c_str());
    HordeSharedMemory* hsm = (HordeSharedMemory*) accessMemory(log, shmemId, sizeof(HordeSharedMemory));

    // Read formulae and assumptions from other individual blocks of shared memory

    // Set up export and import buffers for clause exchanges
    int* exportBuffer;
    int* importBuffer;
    {
        int maxExportBufferSize = programParams.clauseBufferBaseSize() * sizeof(int);
        exportBuffer = (int*) accessMemory(log, shmemId + ".clauseexport", maxExportBufferSize);
        int maxImportBufferSize = hsm->importBufferMaxSize * sizeof(int);
        importBuffer = (int*) accessMemory(log, shmemId + ".clauseimport", maxImportBufferSize);
    }

    // Signal initialization to parent
    hsm->isSpawned = true;
    
    Checksum* checksum = programParams.useChecksums() ? new Checksum() : nullptr;

    // Prepare solver
    HordeLib hlib(programParams, config, log.copy("H", "H"));

    // Wait until everything is prepared for the solver to begin
    while (!hsm->doBegin) doSleep(log);
    
    // Terminate directly?
    if (hsm->doTerminate) doTerminate(log, hsm);

    // Import first revision
    {
        int* fPtr = (int*) accessMemory(log, shmemId + ".formulae.0", sizeof(int) * hsm->fSize);
        int* aPtr = (int*) accessMemory(log, shmemId + ".assumptions.0", sizeof(int) * hsm->aSize);
        hlib.appendRevision(0, hsm->fSize, fPtr, hsm->aSize, aPtr);
        updateChecksum(checksum, fPtr, hsm->fSize);
    }
    // Import subsequent revisions
    int lastImportedRevision = 0;
    while (lastImportedRevision < hsm->firstRevision) {
        if (hsm->doTerminate) doTerminate(log, hsm);
        if (hsm->doStartNextRevision && !hsm->didStartNextRevision) {
            lastImportedRevision++;
            importRevision(log, hlib, shmemId, lastImportedRevision, checksum);
            hsm->didStartNextRevision = true;
        } else doSleep(log);
        if (!hsm->doStartNextRevision) hsm->didStartNextRevision = false;
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

        doSleep(log);

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
            hsm->hasSolution = false;
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

        // Check if clauses should be imported (must not be "from the future")
        if (hsm->doImport && !hsm->didImport && hsm->importBufferRevision <= lastImportedRevision) {
            log.log(V5_DEBG, "DO import clauses\n");
            // Write imported clauses from shared memory into vector
            assert(hsm->importBufferSize <= hsm->importBufferMaxSize);
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
            // Solution found!
            auto& result = hlib.getResult();
            result.id = config.jobid;
            assert(result.revision == lastImportedRevision);
            if (hsm->doTerminate || (hsm->doStartNextRevision && !hsm->didStartNextRevision)) {
                // Result obsolete: there is already another revision
                continue;
            }

            log.log(V5_DEBG, "DO write solution\n");
            solutionVec = result.solution;
            hsm->solutionRevision = result.revision;
            hsm->result = SatResult(result.result);
            size_t* solutionSize = (size_t*) SharedMemory::create(shmemId + ".solutionsize." + std::to_string(hsm->solutionRevision), sizeof(size_t));
            *solutionSize = solutionVec.size();
            // Write solution
            if (*solutionSize > 0) {
                solutionShmemId = shmemId + ".solution." + std::to_string(hsm->solutionRevision);
                solutionShmemSize =  *solutionSize*sizeof(int);
                solutionShmem = (char*) SharedMemory::create(solutionShmemId, solutionShmemSize);
                memcpy(solutionShmem, solutionVec.data(), solutionShmemSize);
            }
            lastSolvedRevision = lastImportedRevision;
            log.log(V5_DEBG, "DONE write solution\n");
            hsm->hasSolution = true;
        }
    }

    doTerminate(log, hsm);

    // Shared memory will be cleaned up by the parent process.
}

int main(int argc, char *argv[]) {
    
    Parameters params;
    params.init(argc, argv);
    HordeConfig config(params.hordeConfig());

    Timer::init(config.starttime);

    int rankOfParent = config.mpirank;

    Random::init(config.mpisize, rankOfParent);

    // Initialize signal handlers
    Process::init(rankOfParent, /*leafProcess=*/true);

    std::string logdir = params.logDirectory();
    Logger::init(rankOfParent, params.verbosity(), params.coloredOutput(), 
            params.quiet(), /*cPrefix=*/params.monoFilename.isSet(),
            params.logToFiles() ? &logdir : nullptr);
    
    auto log = getLog(params, config);
    pid_t pid = Proc::getPid();
    log.log(V3_VERB, "mallob SAT engine %s pid=%lu\n", MALLOB_VERSION, pid);
    if (params.verbosity() >= V5_DEBG) {
        params.printParams();
    }

    try {
        // Launch program
        runSolverEngine(log, params, config);

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
