
#include <iostream>
#include <set>

#include "mpi.h"
#include "worker.h"
#include "client.h"
#include "random.h"

void doExternalClientProgram(MPI_Comm commClients, const std::set<int>& clientRanks) {

    Client client(commClients, clientRanks);
    client.init();
    client.mainProgram();
}

void doWorkerNodeProgram(MPI_Comm commWorkers, const std::set<int>& clientRanks) {

    Worker worker(commWorkers, clientRanks);
    worker.init();
    worker.mainProgram();
}

int main(int argc, char *argv[]) {

    MyMpi::init(argc, argv);

    MyMpi::log("Launching.");

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
        doExternalClientProgram(newComm, externalClientRanks);
    } else {
        doWorkerNodeProgram(newComm, externalClientRanks);
    }

    MPI_Finalize();
}
