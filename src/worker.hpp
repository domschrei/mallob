
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
#include "comm/sysstate.hpp"

class Worker {

private:
    MPI_Comm _comm;
    int _world_rank;
    std::set<int> _client_nodes;
    Parameters& _params;
    float _global_timeout;

    JobDatabase _job_db;
    MessageHandler _msg_handler;
    SysState<3> _sys_state;

    std::vector<int> _hop_destinations;

    std::thread _mpi_monitor_thread;
    std::atomic_bool _exiting = false;

public:
    Worker(MPI_Comm comm, Parameters& params, const std::set<int>& _client_nodes) :
        _comm(comm), _world_rank(MyMpi::rank(MPI_COMM_WORLD)), _client_nodes(_client_nodes), 
        _params(params), _job_db(_params, _comm), _sys_state(_comm)
        {
            _global_timeout = _params.getFloatParam("T");
        }

    ~Worker();
    void init();
    void mainProgram();

private:
    void handleNotifyJobAborting(MessageHandlePtr& handle);
    void handleAcceptAdoptionOffer(MessageHandlePtr& handle);
    void handleConfirmJobRevisionDetails(MessageHandlePtr& handle);
    void handleConfirmAdoption(MessageHandlePtr& handle);
    void handleDoExit(MessageHandlePtr& handle);
    void handleRejectOneshot(MessageHandlePtr& handle);
    void handleRequestNode(MessageHandlePtr& handle, bool oneshot);
    void handleSendClientRank(MessageHandlePtr& handle);
    void handleIncrementalJobFinished(MessageHandlePtr& handle);
    void handleInterrupt(MessageHandlePtr& handle);
    void handleSendApplicationMessage(MessageHandlePtr& handle);
    void handleNotifyJobDone(MessageHandlePtr& handle);
    void handleNotifyJobRevision(MessageHandlePtr& handle);
    void handleOfferAdoption(MessageHandlePtr& handle);
    void handleQueryJobResult(MessageHandlePtr& handle);
    void handleQueryJobRevisionDetails(MessageHandlePtr& handle);
    void handleQueryVolume(MessageHandlePtr& handle);
    void handleRejectAdoptionOffer(MessageHandlePtr& handle);
    void handleNotifyResultObsolete(MessageHandlePtr& handle);
    void handleSendJob(MessageHandlePtr& handle);
    void handleSendJobResult(MessageHandlePtr& handle);
    void handleSendJobRevisionData(MessageHandlePtr& handle);
    void handleSendJobRevisionDetails(MessageHandlePtr& handle);
    void handleNotifyJobTerminating(MessageHandlePtr& handle);
    void handleNotifyVolumeUpdate(MessageHandlePtr& handle);
    void handleNotifyNodeLeavingJob(MessageHandlePtr& handle);
    void handleNotifyResultFound(MessageHandlePtr& handle);
    
    void bounceJobRequest(JobRequest& request, int senderRank);
    void updateVolume(int jobId, int demand);
    void interruptJob(int jobId, bool terminate, bool reckless);
    void informClientJobIsDone(int jobId, int clientRank);
    void applyBalancing();
    void timeoutJob(int jobId);
    
    bool checkTerminate(float time);
    void createExpanderGraph();
    int getRandomNonSelfWorkerNode();

    friend void mpiMonitor(Worker* worker);
};

#endif
