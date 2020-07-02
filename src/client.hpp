
#ifndef DOMPASCH_MALLOB_CLIENT
#define DOMPASCH_MALLOB_CLIENT

#include <string>
#include <set>

#include "comm/mympi.hpp"
#include "util/params.hpp"
#include "data/job_description.hpp"
#include "data/epoch_counter.hpp"
#include "util/sys/threading.hpp"

struct JobByArrivalComparator {
    inline bool operator() (const JobDescription& struct1, const JobDescription& struct2) {
        return (struct1.getArrival() < struct2.getArrival());
    }
};

class Client {

private:
    MPI_Comm _comm;
    int _world_rank;
    Parameters& _params;

    std::vector<int> _ordered_job_ids;
    std::map<int, std::string> _job_instances;
    std::map<int, std::shared_ptr<JobDescription>> _jobs; 
    std::set<int> _introduced_job_ids; 
    std::map<int, bool> _job_ready;
    std::map<int, int> _root_nodes;
    Mutex _job_ready_lock;
    volatile int _last_introduced_job_idx;

    std::set<int> _client_ranks;

    std::thread _instance_reader_thread;

    int _num_alive_clients;

public:
    Client(MPI_Comm comm, Parameters& params, std::set<int> clientRanks)
        : _comm(comm), _params(params), _client_ranks(clientRanks) {
        _world_rank = MyMpi::rank(MPI_COMM_WORLD);
        _num_alive_clients = MyMpi::size(comm);
    };
    ~Client();
    void init();
    void mainProgram();

private:
    void readAllInstances();

    bool checkTerminate();
    void checkClientDone();

    void handleRequestBecomeChild(MessageHandlePtr& handle);
    void handleJobDone(MessageHandlePtr& handle);
    void handleAbort(MessageHandlePtr& handle);
    void handleSendJobResult(MessageHandlePtr& handle);
    void handleAckAcceptBecomeChild(MessageHandlePtr& handle);
    void handleQueryJobRevisionDetails(MessageHandlePtr& handle);
    void handleAckJobRevisionDetails(MessageHandlePtr& handle);
    void handleClientFinished(MessageHandlePtr& handle);
    void handleExit(MessageHandlePtr& handle);

    int getMaxNumParallelJobs();
    int getNextIntroduceableJob();
    bool isJobReady(int jobId);
    void introduceJob(std::shared_ptr<JobDescription>& jobPtr);
    void finishJob(int jobId);
    void readInstanceList(std::string& filename);
    void readFormula(std::string& filename, JobDescription& job);
    friend void readAllInstances(Client* client);
};

#endif
