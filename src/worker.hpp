
#ifndef DOMPASCH_CUCKOO_REBALANCER_WORKER
#define DOMPASCH_CUCKOO_REBALANCER_WORKER

#include <set>
#include <chrono>
#include <string>
#include <thread>
#include <memory>

#include "comm/mympi.hpp"
#include "util/params.hpp"
#include "app/job.hpp"
#include "data/job_description.hpp"
#include "data/job_result.hpp"
#include "data/job_transfer.hpp"
#include "data/epoch_counter.hpp"
#include "balancing/balancer.hpp"
#include "comm/message_handler.hpp"
#include "data/job_database.hpp"

class Worker {

private:
    MPI_Comm comm;
    int worldRank;
    std::set<int> clientNodes;
    Parameters& params;

    float globalTimeout;
    EpochCounter epochCounter;

    JobDatabase _job_db;

    float myState[3];
    float systemState[3];
    MPI_Request systemStateReq;
    bool reducingSystemState = false;
    float lastSystemStateReduce = 0;

    std::vector<int> bounceAlternatives;

    MessageHandler msgHandler;
    std::thread mpiMonitorThread;
    volatile bool exiting;

public:
    Worker(MPI_Comm comm, Parameters& params, const std::set<int>& clientNodes) :
        comm(comm), worldRank(MyMpi::rank(MPI_COMM_WORLD)), clientNodes(clientNodes), params(params), epochCounter(),
        _job_db(params, epochCounter, comm)
        {
            globalTimeout = params.getFloatParam("T");
            
            exiting = false;
            myState[0] = 0.0f;
            myState[1] = 0.0f;
            myState[2] = 0.0f;
        }

    ~Worker();
    void init();
    void mainProgram();
    void dumpStats() {//stats.dump();
    };

private:
    
    void handleAbort(MessageHandlePtr& handle);
    void handleAcceptAdoptionOffer(MessageHandlePtr& handle);
    void handleAckJobRevisionDetails(MessageHandlePtr& handle);
    void handleConfirmAdoption(MessageHandlePtr& handle);
    void handleExit(MessageHandlePtr& handle);
    void handleDeclineOneshot(MessageHandlePtr& handle);
    void handleFindNode(MessageHandlePtr& handle, bool oneshot);
    void handleForwardClientRank(MessageHandlePtr& handle);
    void handleIncrementalJobFinished(MessageHandlePtr& handle);
    void handleInterrupt(MessageHandlePtr& handle);
    void handleJobCommunication(MessageHandlePtr& handle);
    void handleJobDone(MessageHandlePtr& handle);
    void handleNotifyJobRevision(MessageHandlePtr& handle);
    void handleOfferAdoption(MessageHandlePtr& handle);
    void handleQueryJobResult(MessageHandlePtr& handle);
    void handleQueryJobRevisionDetails(MessageHandlePtr& handle);
    void handleQueryVolume(MessageHandlePtr& handle);
    void handleRejectAdoptionOffer(MessageHandlePtr& handle);
    void handleResultObsolete(MessageHandlePtr& handle);
    void handleSendJob(MessageHandlePtr& handle);
    void handleSendJobResult(MessageHandlePtr& handle);
    void handleSendJobRevisionData(MessageHandlePtr& handle);
    void handleSendJobRevisionDetails(MessageHandlePtr& handle);
    void handleTerminate(MessageHandlePtr& handle);
    void handleUpdateVolume(MessageHandlePtr& handle);
    void handleWorkerDefecting(MessageHandlePtr& handle);
    void handleWorkerFoundResult(MessageHandlePtr& handle);
    
    void bounceJobRequest(JobRequest& request, int senderRank);
    void updateVolume(int jobId, int demand);
    void interruptJob(int jobId, bool terminate, bool reckless);
    void informClientJobIsDone(int jobId, int clientRank);
    void applyBalancing();
    void timeoutJob(int jobId);
    
    bool checkTerminate();
    void createExpanderGraph();
    void allreduceSystemState(float elapsedTime = Timer::elapsedSeconds());
    int getRandomNonSelfWorkerNode();

    friend void mpiMonitor(Worker* worker);
};

#endif
