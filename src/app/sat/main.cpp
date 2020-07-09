
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
#include "util/console.hpp"
#include "util/params.hpp"
#include "util/sys/shared_memory.hpp"
#include "util/sys/fork.hpp"
#include "util/sys/proc.hpp"

#include "app/sat/horde_process_adapter.hpp"
#include "app/sat/console_horde_interface.hpp"
#include "hordesat/horde.hpp"

#ifndef MALLOB_VERSION
#define MALLOB_VERSION "(dbg)"
#endif

std::shared_ptr<LoggingInterface> getLog(const Parameters& params) {
    return std::shared_ptr<LoggingInterface>(new ConsoleHordeInterface(
            "<h-" + params.getParam("jobstr") + ">", "#" + params.getParam("jobid") + "."));
}

void runSolverEngine(const std::shared_ptr<LoggingInterface>& log, const Parameters& programParams) {
    
    // Set up "management" block of shared memory created by the parent
    std::string shmemId = "/edu.kit.iti.mallob." + std::to_string(Proc::getParentPid()) + "." + programParams.getParam("mpirank") + ".#" + programParams.getParam("jobid");
    log->log(Console::VERB, "Access base shmem: %s", shmemId.c_str());
    HordeSharedMemory* hsm = (HordeSharedMemory*) SharedMemory::access(shmemId, sizeof(HordeSharedMemory));
    assert(hsm != nullptr);

    // Read formulae and assumptions from other individual blocks of shared memory
    std::vector<std::shared_ptr<std::vector<int>>> formulae;
    int fIdx = 0;
    while (programParams.isSet("fbufsize" + std::to_string(fIdx))) {
        int fSize = programParams.getIntParam("fbufsize" + std::to_string(fIdx));
        std::string fId = shmemId + ".formulae." + std::to_string(fIdx);
        int* fPtr = (int*) SharedMemory::access(fId, fSize);
        formulae.emplace_back(new std::vector<int>(fPtr, fPtr+(fSize/sizeof(int))));
        SharedMemory::free(fId, (char*)fPtr, fSize);
        fIdx++;
    }
    std::shared_ptr<std::vector<int>> assumptions;
    if (programParams.isSet("asmptbufsize")) {
        int aSize = programParams.getIntParam("asmptbufsize");
        std::string aId = shmemId + ".assumptions";
        int* aPtr = (int*) SharedMemory::access(aId, aSize);
        assumptions.reset(new std::vector<int>(aPtr, aPtr+(aSize/sizeof(int))));
        SharedMemory::free(aId, (char*)aPtr, aSize);
    }

    // Set up export and import buffers for clause exchanges
    int maxExportBufferSize = programParams.getIntParam("cbbs") * sizeof(int);
    int* exportBuffer = (int*) SharedMemory::access(shmemId + ".clauseexport", maxExportBufferSize);
    int maxImportBufferSize = programParams.getIntParam("cbbs") * sizeof(int) * programParams.getIntParam("mpisize");
    int* importBuffer = (int*) SharedMemory::access(shmemId + ".clauseimport", maxImportBufferSize);

    // Signal initialization to parent
    pid_t pid = Proc::getPid();
    log->log(1, "Hello from child\n");
    hsm->isSpawned = true;
    
    // Prepare solver
    HordeLib hlib(programParams, log);
    hlib.beginSolving(formulae, assumptions);
    bool interrupted = false;
    std::vector<int> solutionVec;

    std::string solutionShmemId = "";
    char* solutionShmem;
    int solutionShmemSize;

    // Main loop
    while (true) {

        // Wait until something happens
        // (can be interrupted by Fork::wakeUp(hsm->childPid))
        float time = Timer::elapsedSeconds();
        int sleepStatus = usleep(1000 /*1 millisecond*/);
        time = Timer::elapsedSeconds() - time;
        if (sleepStatus != 0) log->log(3, "Woken up after %i us\n", (int) (1000*1000*time));

        // Terminate
        if (hsm->doTerminate) {
            log->log(3, "DO terminate\n");
            break;
        }

        // Interrupt solvers
        if (hsm->doInterrupt && !hsm->didInterrupt) {
            log->log(3, "DO interrupt\n");
            hlib.interrupt();
            hsm->didInterrupt = true;
            interrupted = true;
        }
        if (!hsm->doInterrupt) hsm->didInterrupt = false;

        // Dump stats
        if (!interrupted && hsm->doDumpStats && !hsm->didDumpStats) {
            log->log(3, "DO dump stats\n");
            
            // For this management thread
            double perc_cpu; float sysShare;
            bool success = Proc::getThreadCpuRatio(Proc::getTid(), perc_cpu, sysShare);
            if (success) {
                log->log(0, "child_main : %.2f%% CPU -> %.2f%% systime", perc_cpu, sysShare*100);
            }

            // For each solver thread
            std::vector<long> threadTids = hlib.getSolverTids();
            for (int i = 0; i < threadTids.size(); i++) {
                if (threadTids[i] < 0) continue;
                
                success = Proc::getThreadCpuRatio(threadTids[i], perc_cpu, sysShare);
                if (success) {
                    log->log(0, "td.%ld : %.2f%% CPU -> %.2f%% systime", threadTids[i], perc_cpu, sysShare*100);
                }
            }

            hsm->didDumpStats = true;
        }
        if (!hsm->doDumpStats) hsm->didDumpStats = false;

        // Update role
        if (hsm->doUpdateRole && !hsm->didUpdateRole) {
            log->log(3, "DO update role\n");
            hlib.updateRole(hsm->portfolioRank, hsm->portfolioSize);
            hsm->didUpdateRole = true;
        }
        if (!hsm->doUpdateRole) hsm->didUpdateRole = false;

        // Check if clauses should be exported
        if (!interrupted && hsm->doExport && !hsm->didExport) {
            log->log(3, "DO export clauses\n");
            // Collect local clauses, put into shared memory
            hsm->exportBufferTrueSize = hlib.prepareSharing(exportBuffer, hsm->exportBufferMaxSize);
            hsm->didExport = true;
        }
        if (!hsm->doExport) hsm->didExport = false;

        // Check if clauses should be imported
        if (!interrupted && hsm->doImport && !hsm->didImport) {
            log->log(3, "DO import clauses\n");
            // Write imported clauses from shared memory into vector
            hlib.digestSharing(importBuffer, hsm->importBufferSize);
            hsm->didImport = true;
        }
        if (!hsm->doImport) hsm->didImport = false;

        // Check initialization state
        if (!interrupted && !hsm->isInitialized && hlib.isFullyInitialized()) {
            log->log(3, "DO set initialized\n");
            hsm->isInitialized = true;
        }

        // Check solved state
        if (interrupted || hsm->hasSolution) continue;
        int result = hlib.solveLoop();
        if (result >= 0) {
            log->log(3, "DO write solution\n");
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
            log->log(3, "DONE write solution\n");
            hsm->hasSolution = true;
            continue;
        }
    }

    SharedMemory::free(shmemId + ".clauseexport", (char*)exportBuffer, maxExportBufferSize);
    SharedMemory::free(shmemId + ".clauseimport", (char*)importBuffer, maxImportBufferSize);
    if (!solutionShmemId.empty()) SharedMemory::free(solutionShmemId, solutionShmem, solutionShmemSize);
    hsm->didTerminate = true;
    SharedMemory::free(shmemId, (char*)hsm, sizeof(HordeSharedMemory));
    
    // Exit normally, but avoid calling destructors (some threads may be unresponsive)
    raise(SIGTERM);
}



int main(int argc, char *argv[]) {
    
    Parameters params;
    params.init(argc, argv);
    
    Timer::init(params.getDoubleParam("starttime"));

    int rankOfParent = params.getIntParam("mpirank");

    Console::init(rankOfParent, params.getIntParam("v"), params.isSet("colors"), 
            /*threadsafeOutput=*/false, /*quiet=*/params.isSet("q"), 
            /*cPrefix=*/params.isSet("mono"), params.getParam("log"));
    
    auto log = getLog(params);
    log->log(Console::VERB, "Launching SAT engine %s", MALLOB_VERSION);

    // Initialize signal handlers
    Fork::init(rankOfParent);

    try {
        // Launch program
        runSolverEngine(log, params);

    } catch (const std::exception &ex) {
        log->log(Console::CRIT, "Unexpected ERROR: \"%s\" - aborting", ex.what());
        Console::forceFlush();
        exit(1);
    } catch (...) {
        log->log(Console::CRIT, "Unexpected ERROR - aborting");
        Console::forceFlush();
        exit(1);
    }

    Console::log(Console::INFO, "Exiting");
    Console::flush();
}
