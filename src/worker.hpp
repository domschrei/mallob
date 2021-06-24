
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

#define SYSSTATE_BUSYRATIO 0
#define SYSSTATE_NUMJOBS 1
#define SYSSTATE_GLOBALMEM 2
#define SYSSTATE_NUMHOPS 3
#define SYSSTATE_SPAWNEDREQUESTS 4

class Worker {

private:
    MPI_Comm _comm;
    int _world_rank;
    std::set<int> _client_nodes;
    Parameters& _params;
    float _global_timeout;

    JobDatabase _job_db;
    MessageHandler _msg_handler;
    SysState<5> _sys_state;

    std::vector<int> _hop_destinations;

    std::thread _mpi_monitor_thread;

public:
    Worker(MPI_Comm comm, Parameters& params, const std::set<int>& _client_nodes) :
        _comm(comm), _world_rank(MyMpi::rank(MPI_COMM_WORLD)), _client_nodes(_client_nodes), 
        _params(params), _job_db(_params, _comm), _sys_state(_comm)
        {
            _global_timeout = _params.timeLimit();
        }

    ~Worker();
    void init();
    void mainProgram();

private:
    void handleNotifyJobAborting(MessageHandle& handle);
    void handleAcceptAdoptionOffer(MessageHandle& handle);
    void handleConfirmAdoption(MessageHandle& handle);
    void handleDoExit(MessageHandle& handle);
    void handleRejectOneshot(MessageHandle& handle);
    void handleRequestNode(MessageHandle& handle, bool oneshot);
    void handleSendClientRank(MessageHandle& handle);
    void handleIncrementalJobFinished(MessageHandle& handle);
    void handleInterrupt(MessageHandle& handle);
    void handleSendApplicationMessage(MessageHandle& handle);
    void handleNotifyJobDone(MessageHandle& handle);
    void handleOfferAdoption(MessageHandle& handle);
    void handleQueryJobResult(MessageHandle& handle);
    void handleQueryVolume(MessageHandle& handle);
    void handleRejectAdoptionOffer(MessageHandle& handle);
    void handleNotifyResultObsolete(MessageHandle& handle);
    void handleSendJob(MessageHandle& handle);
    void handleSendJobResult(MessageHandle& handle);
    void handleNotifyJobTerminating(MessageHandle& handle);
    void handleNotifyVolumeUpdate(MessageHandle& handle);
    void handleNotifyNodeLeavingJob(MessageHandle& handle);
    void handleNotifyResultFound(MessageHandle& handle);
    
    void initJob(int jobId, const std::shared_ptr<std::vector<uint8_t>>& data, int senderRank);
    void restartJob(int jobId, const std::shared_ptr<std::vector<uint8_t>>& data, int senderRank);
    void bounceJobRequest(JobRequest& request, int senderRank);
    void updateVolume(int jobId, int demand, int balancingEpoch);
    void interruptJob(int jobId, bool terminate, bool reckless);
    void informClientJobIsDone(int jobId, int clientRank);
    void applyBalancing();
    void timeoutJob(int jobId);
    
    bool checkTerminate(float time);
    void createExpanderGraph();
    int getRandomNonSelfWorkerNode();
};

#endif
