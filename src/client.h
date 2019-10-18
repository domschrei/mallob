
#ifndef DOMPASCH_CUCKOO_REBALANCER_CLIENT
#define DOMPASCH_CUCKOO_REBALANCER_CLIENT

#include <string>
#include <mutex>

#include "util/mpi.h"
#include "util/params.h"
#include "data/job_description.h"
#include "data/statistics.h"
#include "data/epoch_counter.h"

struct JobByArrivalComparator {
    inline bool operator() (const JobDescription& struct1, const JobDescription& struct2) {
        return (struct1.getArrival() < struct2.getArrival());
    }
};

class Client {

private:
    MPI_Comm comm;
    int worldRank;
    Parameters& params;
    Statistics stats;

    std::vector<JobDescription> jobs;
    std::map<int, std::string> jobInstances;
    std::map<int, JobDescription*> introducedJobs; 
    std::map<int, bool> jobReady;
    std::mutex jobReadyLock;

    std::set<int> clientRanks;

    std::thread instanceReaderThread;

public:
    Client(MPI_Comm comm, Parameters& params, std::set<int> clientRanks)
        : comm(comm), params(params), stats(EpochCounter()), clientRanks(clientRanks) {
        this->worldRank = MyMpi::rank(MPI_COMM_WORLD);
    };
    ~Client();
    void init();
    void mainProgram();

private:
    void handleRequestBecomeChild(MessageHandlePtr handle);
    void handleJobDone(MessageHandlePtr handle);
    void handleSendJobResult(MessageHandlePtr handle);
    void handleAckAcceptBecomeChild(MessageHandlePtr handle);

    void readInstanceList(std::string& filename);
    void readFormula(std::string& filename, JobDescription& job);
    friend void readAllInstances(Client* client);
};

#endif
