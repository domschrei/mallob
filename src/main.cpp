
#include <iostream>
#include <set>

#include "util/timer.h"
#include "util/mpi.h"
#include "util/console.h"
#include "util/random.h"
#include "util/params.h"
#include "worker.h"
#include "client.h"

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

    Timer::init();
    MyMpi::init(argc, argv);

    int numNodes = MyMpi::size(MPI_COMM_WORLD);
    int rank = MyMpi::rank(MPI_COMM_WORLD);

    Parameters params;
    if (argc <= 1) {
        if (rank == 0)
            params.printUsage();
        MPI_Finalize();
        exit(0);
    }
    params.init(argc, argv);

    Console::init(rank, params.getIntParam("v"));
    Console::log(Console::VERB, "Launching.");
    params.printParams();

    if (numNodes < 2) {
        Console::log(Console::CRIT, "At least two threads / nodes are necessary in order to run this application.");
        MPI_Finalize();
        exit(0);
    }

    Random::init(rank);

    // Find client ranks
    std::set<int> externalClientRanks;
    int numClients = params.getIntParam("c");
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

    // Launch node's main program
    if (isExternalClient) {
        doExternalClientProgram(newComm, params, externalClientRanks);
    } else {
        doWorkerNodeProgram(newComm, params, externalClientRanks);
    }

    MPI_Finalize();
}
