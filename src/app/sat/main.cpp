
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

void runSolverEngine(const Logger& log, const Parameters& programParams) {
    
    // Set up "management" block of shared memory created by the parent
    std::string shmemId = "/edu.kit.iti.mallob." + std::to_string(Proc::getParentPid()) + "." + programParams.getParam("mpirank") + ".#" + programParams.getParam("jobid");
    log.log(V4_VVER, "Access base shmem: %s\n", shmemId.c_str());
    HordeSharedMemory* hsm = (HordeSharedMemory*) accessMemory(log, shmemId, sizeof(HordeSharedMemory));

    // Read formulae and assumptions from other individual blocks of shared memory
    int fSize = programParams.getIntParam("fbufsize0");
    std::string fId = shmemId + ".formulae.0";
    int* fPtr = (int*) accessMemory(log, fId, fSize);
    int aSize = programParams.getIntParam("asmptbufsize");
    std::string aId = shmemId + ".assumptions";
    int* aPtr = (int*) accessMemory(log, aId, aSize);

    // Set up export and import buffers for clause exchanges
    int maxExportBufferSize = programParams.getIntParam("cbbs") * sizeof(int);
    int* exportBuffer = (int*) accessMemory(log, shmemId + ".clauseexport", maxExportBufferSize);
    int maxImportBufferSize = programParams.getIntParam("cbbs") * sizeof(int) * programParams.getIntParam("mpisize");
    int* importBuffer = (int*) accessMemory(log, shmemId + ".clauseimport", maxImportBufferSize);

    // Signal initialization to parent
    hsm->isSpawned = true;
    
    // Prepare solver
    HordeLib hlib(programParams, log.copy("H", "H"));
    hlib.beginSolving(fSize/sizeof(int), fPtr, aSize/sizeof(int), aPtr);
    bool interrupted = false;
    std::vector<int> solutionVec;

    std::string solutionShmemId = "";
    char* solutionShmem;
    int solutionShmemSize = 0;

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

        // Interrupt solvers
        if (hsm->doInterrupt && !hsm->didInterrupt) {
            log.log(V5_DEBG, "DO interrupt\n");
            hlib.interrupt();
            hsm->didInterrupt = true;
            interrupted = true;
        }
        if (!hsm->doInterrupt) hsm->didInterrupt = false;

        // Dump stats
        if (!interrupted && hsm->doDumpStats && !hsm->didDumpStats) {
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

        // Update role
        if (hsm->doUpdateRole && !hsm->didUpdateRole) {
            log.log(V5_DEBG, "DO update role\n");
            hlib.updateRole(hsm->portfolioRank, hsm->portfolioSize);
            hsm->didUpdateRole = true;
        }
        if (!hsm->doUpdateRole) hsm->didUpdateRole = false;

        // Check if clauses should be exported
        if (!interrupted && hsm->doExport && !hsm->didExport) {
            log.log(V5_DEBG, "DO export clauses\n");
            // Collect local clauses, put into shared memory
            hsm->exportBufferTrueSize = hlib.prepareSharing(exportBuffer, hsm->exportBufferMaxSize);
            hsm->didExport = true;
        }
        if (!hsm->doExport) hsm->didExport = false;

        // Check if clauses should be imported
        if (!interrupted && hsm->doImport && !hsm->didImport) {
            log.log(V5_DEBG, "DO import clauses\n");
            // Write imported clauses from shared memory into vector
            hlib.digestSharing(importBuffer, hsm->importBufferSize);
            hsm->didImport = true;
        }
        if (!hsm->doImport) hsm->didImport = false;

        // Check initialization state
        if (!interrupted && !hsm->isInitialized && hlib.isFullyInitialized()) {
            log.log(V5_DEBG, "DO set initialized\n");
            hsm->isInitialized = true;
        }

        // Check solved state
        if (interrupted || hsm->hasSolution) continue;
        int result = hlib.solveLoop();
        if (result >= 0) {
            log.log(V5_DEBG, "DO write solution\n");
            // Solution found!
            if (result == SatResult::SAT) {
                // SAT
                hsm->result = SAT;
                solutionVec = hlib.getTruthValues();
            } else if (result == SatResult::UNSAT) {
                // UNSAT
                hsm->result = UNSAT; 
                std::set<int> fa = hlib.getFailedAssumptions();
                for (int x : fa) solutionVec.push_back(x);
            }
            // Write solution
            hsm->solutionSize = solutionVec.size();
            if (hsm->solutionSize > 0) {
                solutionShmemId = shmemId + ".solution";
                solutionShmemSize =  hsm->solutionSize*sizeof(int);
                solutionShmem = (char*) SharedMemory::create(solutionShmemId, solutionShmemSize);
                memcpy(solutionShmem, solutionVec.data(), solutionShmemSize);
            }
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
