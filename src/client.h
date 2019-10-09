
#ifndef DOMPASCH_CUCKOO_REBALANCER_CLIENT
#define DOMPASCH_CUCKOO_REBALANCER_CLIENT

#include <string>
#include <mutex>

#include "util/mpi.h"
#include "util/params.h"
#include "data/job_description.h"

class Client {

private:
    MPI_Comm comm;
    int worldRank;
    Parameters& params;

    std::map<float, JobDescription> jobsByArrival;
    std::map<int, std::string> jobInstances;
    std::map<int, bool> jobReady;
    std::mutex jobReadyLock;

    std::set<int> clientRanks;

    std::thread instanceReaderThread;

public:
    Client(MPI_Comm comm, Parameters& params, std::set<int> clientRanks)
        : comm(comm), params(params), clientRanks(clientRanks) {
        this->worldRank = MyMpi::rank(MPI_COMM_WORLD);
    };
    ~Client();
    void init();
    void mainProgram();

private:
    void readInstanceList(std::string& filename);
    void readFormula(std::string& filename, JobDescription& job);
    friend void readAllInstances(Client* client);
};

#endif
