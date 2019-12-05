
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
  void *array[10];
  size_t size;

  // get void*'s for all entries on the stack
  size = backtrace(array, 10);

  // print out all the frames to stderr
  fprintf(stderr, "Error: signal %d:\n", sig);
  backtrace_symbols_fd(array, size, STDERR_FILENO);
  exit(1);
}

void warmUpRun() {
    
    int worldRank = MyMpi::rank(MPI_COMM_WORLD);
    int n = MyMpi::size(MPI_COMM_WORLD);

    // Send
    for (int r = 0; r < n; r++) {
        if (worldRank == r) continue;
        IntVec payload({1, 2, 3, 4, 5, 6, 7, 8});
        MyMpi::isend(MPI_COMM_WORLD, r, MSG_WARMUP, payload);
    }

    // Test and receive
    int received = 0;
    MyMpi::irecv(MPI_COMM_WORLD, MSG_WARMUP);
    while (true) {
        MyMpi::testSentHandles();
        MessageHandlePtr handle = MyMpi::poll();
        if (handle != NULL && handle->tag == MSG_WARMUP) {
            Console::log_recv(Console::VVVERB, handle->source, "Received warmup msg");
            received++;
            if (received == n-1) break;
            else MyMpi::irecv(MPI_COMM_WORLD, MSG_WARMUP);
        }
    }

    Console::log(Console::VERB, "Finished warmup run.");
}

void doExternalClientProgram(MPI_Comm commClients, Parameters& params, const std::set<int>& clientRanks) {
    
    Client client(commClients, params, clientRanks);
    client.init();
    client.mainProgram();
}

void doWorkerNodeProgram(MPI_Comm commWorkers, Parameters& params, const std::set<int>& clientRanks) {

    Worker worker(commWorkers, params, clientRanks);
    worker.init();
    worker.mainProgram();
}

int main(int argc, char *argv[]) {
    //signal(SIGSEGV, handler);

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
    if (params.isSet("h") || params.isSet("help") || params.getFilename().size() == 0) {
        if (rank == 0) {
            params.printUsage();
        }
        MPI_Finalize();
        exit(0);
    }

    char hostname[1024];
	gethostname(hostname, 1024);
    Console::log(Console::VERB, "Launching mallob, revision %s, on %s", MALLOB_REVISION, hostname);

    if (numNodes < 2) {
        Console::log(Console::CRIT, "At least two threads / nodes are necessary in order to run this application.");
        MPI_Finalize();
        exit(0);
    }

    Random::init(rank);

    // Find client ranks
    std::set<int> externalClientRanks;
    int numClients = params.getIntParam("c");
    assert((int)numNodes-numClients > 0 || Console::fail("Need at least one worker node!"));
    for (int i = 1; i <= numClients; i++)
        externalClientRanks.insert(numNodes-i);
    bool isExternalClient = (externalClientRanks.find(rank) != externalClientRanks.end());

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

    std::set_terminate(Console::flush);

    if (params.isSet("warmup") && !params.isSet("derandomize")) {
        // Do global warmup run with explicit all-to-all message passing
        warmUpRun();
    }

    // Launch node's main program
    if (isExternalClient) {
        doExternalClientProgram(newComm, params, externalClientRanks);
    } else {
        doWorkerNodeProgram(newComm, params, externalClientRanks);
    }

    MPI_Finalize();
    Console::log(Console::INFO, "Exiting normally.");
    Console::flush();
}
