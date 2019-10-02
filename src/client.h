
#ifndef DOMPASCH_CUCKOO_REBALANCER_CLIENT
#define DOMPASCH_CUCKOO_REBALANCER_CLIENT

#include <string>
#include <mutex>

#include "mpi.h"
#include "job.h"

class Client {

private:
    MPI_Comm comm;
    int worldRank;

    std::map<float, Job> jobsByArrival;
    std::map<int, std::string> jobInstances;
    std::map<int, bool> jobReady;
    std::mutex jobReadyLock;

    std::set<int> clientRanks;

    std::thread instanceReaderThread;

public:
    Client(MPI_Comm comm, std::set<int> clientRanks)
        : comm(comm), clientRanks(clientRanks) {
        this->worldRank = MyMpi::rank(MPI_COMM_WORLD);
    };
    ~Client();
    void init();
    void mainProgram();

private:
    void readInstanceList(std::string& filename);
    void readFormula(std::string& filename, Job& job);
    friend void readAllInstances(Client* client);
};

#endif
