
#include <iostream>
#include <set>

#include "mpi.h"
#include "worker.h"
#include "client.h"
#include "random.h"
#include "params.h"

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

void printUsage() {

    MyMpi::log("Usage: mallob [-p=<rebalance-period>] [-l=<load-factor>] [-c=<num-clients>] <scenario>");
    MyMpi::log("<scenario>            File path and name prefix for client scenario(s);");
    MyMpi::log("                      will parse <name>.0 for one client, ");
    MyMpi::log("                      <name>.0 and <name>.1 for two clients, ...");
    MyMpi::log("<rebalance-period>    Do global rebalancing every r seconds (r > 0)");
    MyMpi::log("<load-factor>         Load factor to be aimed at (0 < l < 1)");
    MyMpi::log("<num-clients>         Amount of client nodes (int c >= 1)");
}

int main(int argc, char *argv[]) {

    MyMpi::init(argc, argv);

    int numNodes = MyMpi::size(MPI_COMM_WORLD);
    int rank = MyMpi::rank(MPI_COMM_WORLD);

    if (argc <= 1) {
        if (rank == 0)
            printUsage();
        MPI_Finalize();
        exit(0);
    }

    MyMpi::log("Launching.");

    Parameters params;
    params.init(argc, argv);

    if (numNodes < 2) {
        MyMpi::log("At least two threads / nodes are necessary in order to run this application.");
        MPI_Finalize();
        exit(0);
    }

    Random::init(rank);

    std::set<int> externalClientRanks;
    int numClients = params.getIntParam("c", 1);
    for (int i = 1; i <= numClients; i++)
        externalClientRanks.insert(numNodes-i);

    bool isExternalClient = (externalClientRanks.find(rank) != externalClientRanks.end());

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

    if (isExternalClient) {
        doExternalClientProgram(newComm, params, externalClientRanks);
    } else {
        doWorkerNodeProgram(newComm, params, externalClientRanks);
    }

    MPI_Finalize();
}
