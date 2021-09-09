
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
#include "comm/sysstate.hpp"
#include "comm/distributed_bfs.hpp"
#include "util/sys/background_worker.hpp"
#include "balancing/collective_assignment.hpp"
#include "util/periodic_event.hpp"
#include "util/sys/watchdog.hpp"

#define SYSSTATE_BUSYRATIO 0
#define SYSSTATE_NUMJOBS 1
#define SYSSTATE_GLOBALMEM 2
#define SYSSTATE_NUMHOPS 3
#define SYSSTATE_SPAWNEDREQUESTS 4

class Worker {

private:
    MPI_Comm _comm;
    int _world_rank;
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

    long long _iteration = 0;
    PeriodicEvent<3000> _periodic_mem_check;
    PeriodicEvent<10> _periodic_job_check;
    PeriodicEvent<1> _periodic_balance_check;
    PeriodicEvent<1000> _periodic_maintenance;
    Watchdog _watchdog;
    bool _was_idle = true;

public:
    Worker(MPI_Comm comm, Parameters& params);
    ~Worker();
    void init();
    void advance(float time = -1);
    bool checkTerminate(float time);

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
    
    void sendRevisionDescription(int jobId, int revision, int dest);
    void bounceJobRequest(JobRequest& request, int senderRank);
    void initiateVolumeUpdate(int jobId);
    void updateVolume(int jobId, int volume, int balancingEpoch);
    void interruptJob(int jobId, bool terminate, bool reckless);
    void sendJobDoneWithStatsToClient(int jobId, int successfulRank);
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
