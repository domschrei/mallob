
#include <iostream>
#include <set>
#include <exception>
#include <execinfo.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

#include "util/timer.h"
#include "util/mympi.h"
#include "util/console.h"
#include "util/random.h"
#include "util/params.h"
#include "worker.h"
#include "client.h"

#include "revision.c"

void handler(int sig) {
  Console::forceFlush();

  void *array[10];
  size_t size;

  // get void*'s for all entries on the stack
  size = backtrace(array, 10);

  // print out all the frames to stderr
  fprintf(stderr, "Error from tid %ld: signal %d:\n", syscall(SYS_gettid), sig);
  backtrace_symbols_fd(array, size, STDERR_FILENO);
  exit(1);
}

void doExternalClientProgram(MPI_Comm& commClients, Parameters& params, const std::set<int>& clientRanks) {
    
    Client client(commClients, params, clientRanks);
    client.init();
    client.mainProgram();
}

void doWorkerNodeProgram(MPI_Comm& commWorkers, Parameters& params, const std::set<int>& clientRanks) {

    Worker worker(commWorkers, params, clientRanks);
    worker.init();
    worker.mainProgram();
}

int main(int argc, char *argv[]) {
    signal(SIGABRT, handler);
    signal(SIGSEGV, handler);

    Timer::init();
    MyMpi::init(argc, argv);

    int numNodes = MyMpi::size(MPI_COMM_WORLD);
    int rank = MyMpi::rank(MPI_COMM_WORLD);

    Parameters params;
    params.init(argc, argv);
    Console::init(rank, params.getIntParam("v"), params.isSet("colors"), 
            /*threadsafeOutput=*/false, /*quiet=*/params.isSet("q"), params.getParam("log"));
    
    if (rank == 0)
        params.printParams();
    if (params.isSet("h") || params.isSet("help") 
            || (!params.isSet("sinst") && params.getFilename().size() == 0)) {
        // Help requested or no job input provided
        if (rank == 0) {
            params.printUsage();
        }
        MPI_Finalize();
        exit(0);
    }

    char hostname[1024];
	gethostname(hostname, 1024);
    Console::log(Console::VERB, "Launching mallob, revision %s, on %s", MALLOB_REVISION, hostname);

    // Global and local seed, such that all nodes have access to a synchronized randomness
    // as well as to an individual randomness that differs among nodes
    Random::init(numNodes, rank);

    // Find client ranks
    std::set<int> externalClientRanks;
    int numClients = params.getIntParam("c");
    int numWorkers = numNodes - numClients;
    assert(numWorkers > 0 || Console::fail("Need at least one worker node!"));
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

    //std::set_terminate(Console::forceFlush);

    try {
        // Launch node's main program
        if (isExternalClient) {
            doExternalClientProgram(newComm, params, externalClientRanks);
        } else {
            doWorkerNodeProgram(newComm, params, externalClientRanks);
        }
    } catch (const std::exception &ex) {
        Console::log(Console::CRIT, "Unexpected ERROR: \"%s\" - aborting", ex.what());
        Console::forceFlush();
        exit(1);
    } catch (...) {
        Console::log(Console::CRIT, "Unexpected ERROR - aborting");
        Console::forceFlush();
        exit(1);
    }

    MPI_Finalize();
    Console::log(Console::INFO, "Exiting happily");
    Console::flush();
}
