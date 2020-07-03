
#include <iostream>
#include <set>
#include <stdlib.h>
#include <unistd.h>
#include <map>
#include <string>
#include <vector>
#include <memory>

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


std::shared_ptr<LoggingInterface> getLog(Parameters& params) {
    return std::shared_ptr<LoggingInterface>(new ConsoleHordeInterface(
            "<h-" + params.getParam("jobstr") + ">", "#" + params.getParam("jobid") + "."));
}

void runSolverEngine(const std::shared_ptr<LoggingInterface>& log, Parameters& programParams) {

    // Set up parameters for Horde
    std::map<std::string, std::string> params;
    params["e"] = "1"; // exchange mode: 0 = nothing, 1 = alltoall, 2 = log, 3 = asyncrumor
    params["c"] = programParams.getParam("t"); // solver threads on this node
    if (programParams.getIntParam("md") <= 1 && programParams.getIntParam("t") <= 1) {
        // One thread on one node: do not diversify anything, but keep default solver settings
        params["d"] = "0"; // no diversification
    } else if (programParams.isSet("nophase")) {
        // Do not do sparse random ("phase") diversification
        params["d"] = "4"; // native diversification only
    } else {
        params["d"] = "7"; // sparse random + native diversification
    }
    params["fd"]; // filter duplicate clauses
    params["cbbs"] = programParams.getParam("cbbs"); // clause buffer base size
    params["icpr"] = programParams.getParam("icpr"); // increase clause production
    params["mcl"] = programParams.getParam("mcl"); // max clause length
    params["i"] = "0"; // #microseconds to sleep during solve loop
    params["v"] = "99"; // (this->_params.getIntParam("v") >= 3 ? "1" : "0"); // verbosity
    params["mpirank"] = programParams.getParam("mpirank"); // mpi_rank
    params["mpisize"] = programParams.getParam("mpisize"); // mpi_size
    std::string identifier = programParams.getParam("jobstr");
    params["jobstr"] = identifier;
    if (programParams.isSet("sinst")) {
        // Single instance filename
        params["sinst"] = programParams.getParam("sinst");
    }
    params["cfhl"] = programParams.getParam("cfhl");
    if (programParams.isSet("aod")) params["aod"];
    
    // Set up "management" block of shared memory created by the parent
    std::string shmemId = "/edu.kit.mallob." + identifier;
    log->log(Console::VERB, "Access base shmem: %s", shmemId.c_str());
    HordeSharedMemory* hsm = (HordeSharedMemory*) SharedMemory::access(shmemId, sizeof(HordeSharedMemory));
    assert(hsm != nullptr);

    // Read formulae and assumptions from other individual blocks of shared memory
    std::vector<std::shared_ptr<std::vector<int>>> formulae;
    int fIdx = 0;
    while (programParams.isSet("fbufsize" + std::to_string(fIdx))) {
        int fSize = programParams.getIntParam("fbufsize" + std::to_string(fIdx));
        int* fPtr = (int*) SharedMemory::access(shmemId + ".formulae." + std::to_string(fIdx), fSize);
        formulae.emplace_back(new std::vector<int>(fPtr, fPtr+(fSize/sizeof(int))));
        fIdx++;
    }
    std::shared_ptr<std::vector<int>> assumptions;
    if (programParams.isSet("asmptbufsize")) {
        int aSize = programParams.getIntParam("asmptbufsize");
        int* aPtr = (int*) SharedMemory::access(shmemId + ".assumptions", aSize);
        assumptions.reset(new std::vector<int>(aPtr, aPtr+(aSize/sizeof(int))));
    }

    // Set up export and import buffers for clause exchanges
    int maxExportBufferSize = programParams.getIntParam("cbbs") * sizeof(int);
    int* exportBuffer = (int*) SharedMemory::access(shmemId + ".clauseexport", maxExportBufferSize);
    int maxImportBufferSize = programParams.getIntParam("cbbs") * sizeof(int) * programParams.getIntParam("mpisize");
    int* importBuffer = (int*) SharedMemory::access(shmemId + ".clauseimport", maxImportBufferSize);

    // Set up logging interface
    auto log = std::shared_ptr<LoggingInterface>(new ConsoleHordeInterface(
            "<h-" + std::string(identifier) + ">", "#" + programParams.getParam("jobid") + "."));

    // Signal initialization to parent
    pid_t pid = Proc::getPid();
    log->log(1, "Hello from child\n");
    hsm->isSpawned = true;
    
    // Prepare solver
    HordeLib hlib(params, log);
    hlib.beginSolving(formulae, assumptions);
    bool interrupted = false;
    std::vector<int> solutionVec;
    int* solutionShmem;

    // Main loop
    while (true) {

        // Wait until something happens
        // (can be interrupted by Fork::wakeUp(hsm->childPid))
        float time = Timer::elapsedSeconds();
        int sleepStatus = usleep(1000 /*1 millisecond*/);
        time = Timer::elapsedSeconds() - time;
        if (sleepStatus != 0) log->log(3, "Woken up after %i us\n", (int) (1000*1000*time));

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
                solutionShmem = (int*) SharedMemory::create(shmemId + ".solution", hsm->solutionSize*sizeof(int));
                memcpy(solutionShmem, solutionVec.data(), hsm->solutionSize*sizeof(int));
            }
            log->log(3, "DONE write solution\n");
            hsm->hasSolution = true;
            continue;
        }
    }
}



int main(int argc, char *argv[]) {
    

    Parameters params;
    params.init(argc, argv);
    
    Timer::init(params.getDoubleParam("starttime"));

    int rankOfParent = params.getIntParam("rank");

    Console::init(rankOfParent, params.getIntParam("v"), params.isSet("colors"), 
            /*threadsafeOutput=*/false, /*quiet=*/params.isSet("q"), 
            /*cPrefix=*/params.isSet("sinst"), params.getParam("log"));
    
    auto log = getLog(params);
    log->log(Console::VERB, "Launching SAT engine %s", MALLOB_VERSION);

    // Initialize bookkeeping of child processes
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
