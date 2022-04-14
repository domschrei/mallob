
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
#include "data/job_database.hpp"
#include "data/worker_sysstate.hpp"
#include "comm/distributed_bfs.hpp"
#include "util/sys/background_worker.hpp"
#include "balancing/collective_assignment.hpp"
#include "util/periodic_event.hpp"
#include "util/sys/watchdog.hpp"
#include "comm/host_comm.hpp"

/*
Primary actor in the system who is responsible for participating in the scheduling and execution of jobs.
There is at most one Worker instance for each PE.
*/
class Worker {

private:
    MPI_Comm _comm;
    int _world_rank;
    Parameters& _params;

    JobDatabase _job_db;
    WorkerSysState _sys_state;

    std::vector<int> _hop_destinations;
    CollectiveAssignment _coll_assign;

    long long _iteration = 0;
    PeriodicEvent<1000> _periodic_stats_check;
    PeriodicEvent<2990> _periodic_big_stats_check; // ready at every 3rd "ready" of _periodic_stats_check
    PeriodicEvent<10> _periodic_job_check;
    PeriodicEvent<1> _periodic_balance_check;
    PeriodicEvent<1000> _periodic_maintenance;
    Watchdog _watchdog;

    std::atomic_bool _node_stats_calculated = true;
    float _node_memory_gbs = 0;
    double _mainthread_cpu_share = 0;
    float _mainthread_sys_share = 0;
    unsigned long _machine_free_kbs = 0;
    unsigned long _machine_total_kbs = 0;

    robin_hood::unordered_map<std::pair<int, int>, JobResult, IntPairHasher> _pending_results;

    robin_hood::unordered_map<int, int> _send_id_to_job_id;

    HostComm* _host_comm;

public:
    Worker(MPI_Comm comm, Parameters& params);
    ~Worker();
    void init();
    void advance(float time = -1);
    void setHostComm(HostComm& hostComm) {_host_comm = &hostComm;}

private:
    void handleRequestNode(MessageHandle& handle, JobDatabase::JobRequestMode mode);
    void handleOfferAdoption(MessageHandle& handle);
    void handleAnswerAdoptionOffer(MessageHandle& handle);
    void handleQueryJobDescription(MessageHandle& handle);
    void handleSendJobDescription(MessageHandle& handle);

    void handleNotifyJobAborting(MessageHandle& handle);
    void handleDoExit(MessageHandle& handle);
    void handleRejectOneshot(MessageHandle& handle);
    void handleIncrementalJobFinished(MessageHandle& handle);
    void handleInterrupt(MessageHandle& handle);
    void handleSendApplicationMessage(MessageHandle& handle);
    void handleNotifyJobDone(MessageHandle& handle);
    void handleQueryJobResult(MessageHandle& handle);
    void handleQueryVolume(MessageHandle& handle);
    void handleNotifyResultObsolete(MessageHandle& handle);
    void handleNotifyJobTerminating(MessageHandle& handle);
    void handleNotifyVolumeUpdate(MessageHandle& handle);
    void handleNotifyNodeLeavingJob(MessageHandle& handle);
    void handleNotifyResultFound(MessageHandle& handle);
    void handleNotifyNeighborStatus(MessageHandle& handle);
    void handleNotifyNeighborIdleDistance(MessageHandle& handle);
    void handleRequestWork(MessageHandle& handle);
    void handleSchedReleaseFromWaiting(MessageHandle& handle);
    void handleSchedNodeFreed(MessageHandle& handle);

    void sendRevisionDescription(int jobId, int revision, int dest);
    void bounceJobRequest(JobRequest& request, int senderRank);

    void checkStats(float time);
    void checkJobs();
    void checkActiveJob();
    void publishAndResetSysState();

    void tryAdoptRequest(JobRequest& req, int source, JobDatabase::JobRequestMode mode);

    void initiateVolumeUpdate(int jobId);
    void updateVolume(int jobId, int volume, int balancingEpoch, float eventLatency);
    void spawnJobRequest(int jobId, bool left, int balancingEpoch);
    void sendJobRequest(const JobRequest& req, int tag, bool left, int dest);
    void activateRootRequest(int jobId);
    void propagateVolumeUpdate(Job& job, int volume, int balancingEpoch);

    void interruptJob(int jobId, bool terminate, bool reckless);
    void sendJobDoneWithStatsToClient(int jobId, int revision, int successfulRank);
    void timeoutJob(int jobId);

    void sendStatusToNeighbors();
    int getIdleDistance();
    int getWeightedRandomNeighbor();

    void updateNeighborStatus(int rank, bool busy);
    bool isOnlyIdleWorkerInLocalPerimeter();
    
    void createExpanderGraph();
    int getRandomNonSelfWorkerNode();
};

#endif
