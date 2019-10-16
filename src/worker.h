
#ifndef DOMPASCH_CUCKOO_REBALANCER_WORKER
#define DOMPASCH_CUCKOO_REBALANCER_WORKER

#include <set>
#include <chrono>
#include <string>
#include <thread>
#include <memory>

#include "HordeLib.h"

#include "util/mpi.h"
#include "util/params.h"
#include "data/job.h"
#include "data/job_description.h"
#include "data/job_transfer.h"
#include "data/epoch_counter.h"
#include "balancing/balancer.h"

class Worker {

private:
    MPI_Comm comm;
    int worldRank;
    std::set<int> clientNodes;
    Parameters& params;

    float loadFactor;

    std::map<int, Job*> jobs;
    std::map<int, JobRequest> jobCommitments;
    int load;

    std::unique_ptr<Balancer> balancer;
    EpochCounter epochCounter;

public:
    Worker(MPI_Comm comm, Parameters& params, const std::set<int>& clientNodes) :
        comm(comm), worldRank(MyMpi::rank(MPI_COMM_WORLD)), clientNodes(clientNodes), params(params), epochCounter()
        {
            loadFactor = params.getFloatParam("l");
            assert(0 < loadFactor && loadFactor < 1.0);
            load = 0;
        }

    void init();
    void mainProgram();

private:

    void checkTerminate();

    void handleIntroduceJob(MessageHandlePtr& handle);
    void handleFindNode(MessageHandlePtr& handle);
    void handleRequestBecomeChild(MessageHandlePtr& handle);
    void handleRejectBecomeChild(MessageHandlePtr& handle);
    void handleAcceptBecomeChild(MessageHandlePtr& handle);
    void handleAckAcceptBecomeChild(MessageHandlePtr& handle);
    void handleSendJob(MessageHandlePtr& handle);
    void handleUpdateVolume(MessageHandlePtr& handle);
    void handleJobCommunication(MessageHandlePtr& handle);
    void handleTerminate(MessageHandlePtr& handle);
    void handleWorkerFoundResult(MessageHandlePtr& handle);
    void handleQueryJobResult(MessageHandlePtr& handle);
    void handleForwardClientRank(MessageHandlePtr& handle);

    void bounceJobRequest(JobRequest& request);
    void informClient(int jobId, int clientRank);
    void updateVolume(int jobId, int demand);
    
    void rebalance();
    float reduce(float contribution, int rootRank) const;
    float allReduce(float contribution) const;

    int getLoad() const {return load;};
    bool isIdle() const {return load == 0;};
    bool hasJobCommitments() const {return jobCommitments.size() > 0;};
    int getRandomWorkerNode();
    bool isTimeForRebalancing();

    bool hasJob(int id) const {
        return jobs.count(id) > 0;
    }
    Job& getJob(int id) const {
        return *jobs.at(id);
    };

    std::string jobStr(int j, int idx) const {return "#" + std::to_string(j) + ":" + std::to_string(idx);};
};

#endif
