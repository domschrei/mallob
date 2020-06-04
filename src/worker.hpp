
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

class Worker {

private:
    MPI_Comm comm;
    int worldRank;
    std::set<int> clientNodes;
    Parameters& params;

    float loadFactor;
    float globalTimeout;
    float balancePeriod;
    int numThreads;
    float wcSecsPerInstance;
    float cpuSecsPerInstance;

    std::map<int, Job*> jobs;
    std::map<int, JobRequest> jobCommitments;
    std::map<int, float> jobArrivals;
    std::map<int, float> jobCpuTimeUsed;
    std::map<int, float> lastLimitCheck;
    std::map<int, int> jobVolumes;
    std::map<int, std::thread> initializerThreads;

    Job* currentJob;
    int load;
    float lastLoadChange;

    std::unique_ptr<Balancer> balancer;
    EpochCounter epochCounter;

    float myState[3];
    float systemState[3];
    MPI_Request systemStateReq;
    bool reducingSystemState = false;
    float lastSystemStateReduce = 0;

    std::vector<int> bounceAlternatives;

    MessageHandler msgHandler;
    std::thread mpiMonitorThread;
    volatile bool exiting;

    struct SuspendedJobComparator {
        bool operator()(const std::pair<int, float>& left, const std::pair<int, float>& right) {
            return left.second < right.second;
        };
    };

public:
    Worker(MPI_Comm comm, Parameters& params, const std::set<int>& clientNodes) :
        comm(comm), worldRank(MyMpi::rank(MPI_COMM_WORLD)), clientNodes(clientNodes), params(params), epochCounter()
        {
            loadFactor = params.getFloatParam("l");
            assert(0 < loadFactor && loadFactor <= 1.0);
            globalTimeout = params.getFloatParam("T");
            balancePeriod = params.getFloatParam("p");
            numThreads = params.getIntParam("t");
            wcSecsPerInstance = params.getFloatParam("time-per-instance");
            cpuSecsPerInstance = 3600 * params.getFloatParam("cpuh-per-instance");
            load = 0;
            lastLoadChange = Timer::elapsedSeconds();
            currentJob = NULL;
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
    
    Job* createJob(Parameters& params, int commSize, int worldRank, int jobId, EpochCounter& epochCounter);
    void bounceJobRequest(JobRequest& request, int senderRank);
    void updateVolume(int jobId, int demand);
    void interruptJob(int jobId, bool terminate, bool reckless);
    void informClientJobIsDone(int jobId, int clientRank);
    void timeoutJob(int jobId);
    void forgetJob(int jobId);
    void deleteJob(int jobId);
    bool checkComputationLimits(int jobId);
    
    bool isTimeForRebalancing();
    void rebalance();
    void finishBalancing();
    
    bool checkTerminate();
    void createExpanderGraph();
    void allreduceSystemState(float elapsedTime = Timer::elapsedSeconds());
    void forgetOldJobs();

    int getLoad() const {return load;};
    void setLoad(int load, int whichJobId);
    bool isIdle() const {return load == 0;};
    bool hasJobCommitments() const {return jobCommitments.size() > 0;};
    
    int getRandomNonSelfWorkerNode();
    
    bool isRequestObsolete(const JobRequest& req);
    bool isAdoptionOfferObsolete(const JobRequest& req);

    bool hasJob(int id) const {
        return jobs.count(id) > 0;
    }
    Job& getJob(int id) const {
        assert(jobs.count(id));
        return *jobs.at(id);
    };
    std::string jobStr(int j, int idx) const {
        return "#" + std::to_string(j) + ":" + std::to_string(idx);
    };

    friend void mpiMonitor(Worker* worker);
};

#endif
