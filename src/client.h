
#ifndef DOMPASCH_CUCKOO_REBALANCER_CLIENT
#define DOMPASCH_CUCKOO_REBALANCER_CLIENT

#include <string>
#include <mutex>

#include "util/mympi.h"
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

    std::vector<std::shared_ptr<JobDescription>> jobs;
    std::map<int, std::string> jobInstances;
    std::map<int, std::shared_ptr<JobDescription>> introducedJobs; 
    std::map<int, bool> jobReady;
    std::map<int, int> rootNodes;
    std::mutex jobReadyLock;
    int lastIntroducedJobIdx;

    std::set<int> clientRanks;

    std::thread instanceReaderThread;

    int numAliveClients;

public:
    Client(MPI_Comm comm, Parameters& params, std::set<int> clientRanks)
        : comm(comm), params(params), stats(EpochCounter()), clientRanks(clientRanks) {
        this->worldRank = MyMpi::rank(MPI_COMM_WORLD);
        numAliveClients = MyMpi::size(comm);
    };
    ~Client();
    void init();
    void mainProgram();

private:
    bool checkTerminate();
    void checkFinished();

    void handleRequestBecomeChild(MessageHandlePtr& handle);
    void handleJobDone(MessageHandlePtr& handle);
    void handleAbort(MessageHandlePtr& handle);
    void handleSendJobResult(MessageHandlePtr& handle);
    void handleAckAcceptBecomeChild(MessageHandlePtr& handle);
    void handleQueryJobRevisionDetails(MessageHandlePtr& handle);
    void handleAckJobRevisionDetails(MessageHandlePtr& handle);
    void handleClientFinished(MessageHandlePtr& handle);
    void handleExit(MessageHandlePtr& handle);

    bool isJobReady(int jobId);
    void introduceJob(std::shared_ptr<JobDescription>& jobPtr);
    void readInstanceList(std::string& filename);
    void readFormula(std::string& filename, JobDescription& job);
    friend void readAllInstances(Client* client);
};

#endif
