
#ifndef DOMPASCH_MALLOB_WORKER_HPP
#define DOMPASCH_MALLOB_WORKER_HPP

#include <set>
#include <chrono>
#include <string>
#include <memory>

#include "comm/mympi.hpp"
#include "util/params.hpp"
#include "app/job.hpp"
#include "data/job_description.hpp"
#include "data/job_result.hpp"
#include "data/job_transfer.hpp"
#include "balancing/balancer.hpp"
#include "data/job_database.hpp"
#include "comm/sysstate.hpp"
#include "comm/distributed_bfs.hpp"
#include "util/sys/background_worker.hpp"
#include "balancing/collective_assignment.hpp"

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
    SysState<5> _sys_state;

    std::vector<int> _hop_destinations;
    robin_hood::unordered_map<int, int> _neighbor_idle_distance;

    robin_hood::unordered_set<int> _busy_neighbors;
    std::set<WorkRequest, WorkRequestComparator> _recent_work_requests;
    float _time_only_idle_worker = -1;

    DistributedBFS _bfs;
    CollectiveAssignment _coll_assign;

public:
    Worker(MPI_Comm comm, Parameters& params, const std::set<int>& _client_nodes) :
        _comm(comm), _world_rank(MyMpi::rank(MPI_COMM_WORLD)), _client_nodes(_client_nodes), 
        _params(params), _job_db(_params, _comm), _sys_state(_comm), 
        _bfs(_hop_destinations, [this](const JobRequest& req) {
            if (_job_db.isIdle() && !_job_db.hasCommitment(req.jobId)) {
                // Try to adopt this job request
                log(V4_VVER, "BFS Try adopting %s\n", req.toStr().c_str());
                MessageHandle handle;
                handle.receiveSelfMessage(req.serialize(), _world_rank);
                handleRequestNode(handle, JobDatabase::IGNORE_FAIL);
                return _job_db.hasCommitment(req.jobId) ? _world_rank : -1;
            }
            return -1;
        }, [this](const JobRequest& request, int foundRank) {
            if (foundRank == -1) {
                log(V4_VVER, "%s : BFS unsuccessful - continue bouncing\n", request.toStr().c_str());
                JobRequest req = request;
                bounceJobRequest(req, _world_rank);
            } else {
                log(V4_VVER, "%s : BFS successful ([%i] adopting)\n", request.toStr().c_str(), foundRank);
            }
        })
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
    void handleRequestNode(MessageHandle& handle, JobDatabase::JobRequestMode mode);
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
    void handleNotifyNeighborStatus(MessageHandle& handle);
    void handleNotifyNeighborIdleDistance(MessageHandle& handle);
    void handleRequestWork(MessageHandle& handle);
    
    void initJob(int jobId, const std::shared_ptr<std::vector<uint8_t>>& data, int senderRank);
    void restartJob(int jobId, const std::shared_ptr<std::vector<uint8_t>>& data, int senderRank);
    void bounceJobRequest(JobRequest& request, int senderRank);
    void updateVolume(int jobId, int demand, int balancingEpoch);
    void interruptJob(int jobId, bool terminate, bool reckless);
    void informClientJobIsDone(int jobId, int clientRank);
    void applyBalancing();
    void timeoutJob(int jobId);

    void sendStatusToNeighbors();
    int getIdleDistance();
    int getWeightedRandomNeighbor();

    void updateNeighborStatus(int rank, bool busy);
    bool isOnlyIdleWorkerInLocalPerimeter();
    
    bool checkTerminate(float time);
    void createExpanderGraph();
    int getRandomNonSelfWorkerNode();
};

#endif
