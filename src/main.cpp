
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

int main(int argc, char *argv[]) {

    MyMpi::init(argc, argv);

    MyMpi::log("Launching.");

    Parameters params;
    params.init(argc, argv);

    int numNodes = MyMpi::size(MPI_COMM_WORLD);
    int rank = MyMpi::rank(MPI_COMM_WORLD);
    if (numNodes < 2) {
        MyMpi::log("At least two threads / nodes are necessary in order to run this application.");
        exit(1);
    }

    Random::init(rank);

    std::set<int> externalClientRanks;
    externalClientRanks.insert(numNodes-1);

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
