
#include <iostream>
#include <set>
#include <stdlib.h>
#include <unistd.h>

#include "comm/mympi.hpp"
#include "util/sys/timer.hpp"
#include "util/logger.hpp"
#include "util/random.hpp"
#include "util/params.hpp"
#include "util/sys/shared_memory.hpp"
#include "util/sys/process.hpp"
#include "util/sys/proc.hpp"
#include "worker.hpp"
#include "client.hpp"

#ifndef MALLOB_VERSION
#define MALLOB_VERSION "(dbg)"
#endif

void doExternalClientProgram(MPI_Comm& commClients, Parameters& params, const std::set<int>& clientRanks) {
    
    Client client(commClients, params, clientRanks);
    try {
        client.init();
        client.mainProgram();
    } catch (const std::exception& ex) {
        log(V0_CRIT, "Unexpected ERROR: \"%s\" - aborting\n", ex.what());
        Process::doExit(1);
    } catch (...) {
        log(V0_CRIT, "Unexpected ERROR - aborting\n");
        Process::doExit(1);
    }
}

void doWorkerNodeProgram(MPI_Comm& commWorkers, Parameters& params, const std::set<int>& clientRanks) {

    Worker worker(commWorkers, params, clientRanks);
    try {
        worker.init();
        worker.mainProgram();
    } catch (const std::exception& ex) {
        log(V0_CRIT, "Unexpected ERROR: \"%s\" - aborting\n", ex.what());
        Process::doExit(1);
    } catch (...) {
        log(V0_CRIT, "Unexpected ERROR - aborting\n");
        Process::doExit(1);
    }
}

int main(int argc, char *argv[]) {
    
    Timer::init();
    MyMpi::init(argc, argv);

    int numNodes = MyMpi::size(MPI_COMM_WORLD);
    int rank = MyMpi::rank(MPI_COMM_WORLD);

    // Initialize bookkeeping of child processes and signals
    Process::init(rank);

    Parameters params;
    params.init(argc, argv);

    bool quiet = /*quiet=*/params.isNotNull("q");
    if (params.isNotNull("0o") && rank > 0) quiet = true;
    std::string logdir = params.getParam("log");
    Logger::init(rank, params.getIntParam("v"), params.isNotNull("colors"), 
            quiet, /*cPrefix=*/params.isNotNull("mono"), params.isNotNull("nolog") ? nullptr : &logdir);

    MyMpi::setOptions(params);

    if (rank == 0)
        params.printParams();
    if (params.isSet("h") || params.isSet("help")) {
        // Help requested or no job input provided
        if (rank == 0) {
            params.printUsage();
        }
        MPI_Finalize();
        Process::doExit(0);
    }

    char hostname[1024];
	gethostname(hostname, 1024);
    log(V3_VERB, "mallob %s pid=%lu on host %s\n", MALLOB_VERSION, Proc::getPid(), hostname);

    // Global and local seed, such that all nodes have access to a synchronized randomness
    // as well as to an individual randomness that differs among nodes
    Random::init(numNodes, rank);

    // Find client ranks
    std::set<int> externalClientRanks;
    int numClients = params.getIntParam("c");
    int numWorkers = numNodes - numClients;
    assert(numWorkers > 0 || log_return_false("Need at least one worker node!\n"));
    for (int i = 1; i <= numClients; i++)
        externalClientRanks.insert(numNodes-i);
    bool isExternalClient = rank >= numWorkers;

    // Create two disjunct communicators: Clients and workers
    int color = -1;
    if (isExternalClient) {
        // External client node
        color = 1;
    } else {
        // Idle worker node
        color = 2;
    }
    MPI_Comm newComm;
    MPI_Comm_split(MPI_COMM_WORLD, color, rank, &newComm);

    // Launch node's main program
    if (isExternalClient) {
        doExternalClientProgram(newComm, params, externalClientRanks);
    } else {
        doWorkerNodeProgram(newComm, params, externalClientRanks);
    }

    MPI_Finalize();
    log(V2_INFO, "Exiting happily\n");
    Process::doExit(0);
}
