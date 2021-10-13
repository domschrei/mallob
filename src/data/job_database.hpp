
#ifndef DOMPASCH_MALLOB_JOB_DATABASE_HPP
#define DOMPASCH_MALLOB_JOB_DATABASE_HPP

#include <list>

#include "util/hashing.hpp"
#include "app/job.hpp"
#include "job_transfer.hpp"
#include "balancing/event_driven_balancer.hpp"
#include "util/sys/background_worker.hpp"
#include "balancing/collective_assignment.hpp"
#include "data/worker_sysstate.hpp"

class JobDatabase {

private:
    Parameters& _params;
    float _wcsecs_per_instance;
    float _cpusecs_per_instance;
    float _load_factor;
    float _balance_period;

    MPI_Comm& _comm;
    std::unique_ptr<EventDrivenBalancer> _balancer;
    robin_hood::unordered_map<int, int> _current_volumes;
    CollectiveAssignment* _coll_assign = nullptr;

    std::atomic_int _num_stored_jobs = 0;
    robin_hood::unordered_map<int, Job*> _jobs;
    bool _has_commitment = false;

    int _load;
    Job* _current_job;
    float _last_balancing_initiation;

    // Requests which lay dormant (e.g., due to too many hops / too busy system)
    // and will be re-introduced to continue hopping after some time
    std::list<std::tuple<float, int, JobRequest>> _deferred_requests;

    // Request to re-activate a local dormant root
    std::optional<JobRequest> _pending_root_reactivate_request;

    struct SuspendedJobComparator {
        bool operator()(const std::pair<int, float>& left, const std::pair<int, float>& right) {
            return left.second < right.second;
        };
    };

    BackgroundWorker _janitor;
    std::list<Job*> _jobs_to_free;
    Mutex _janitor_mutex;

    WorkerSysState& _sys_state;
    float _total_busy_time = 0;
    float _time_of_last_adoption = 0;

public:
    JobDatabase(Parameters& params, MPI_Comm& comm, WorkerSysState& sysstate);
    ~JobDatabase();
    void setCollectiveAssignment(CollectiveAssignment& collAssign) {_coll_assign = &collAssign;}

    Job& createJob(int commSize, int worldRank, int jobId, JobDescription::Application application);
    bool appendRevision(int jobId, const std::shared_ptr<std::vector<uint8_t>>& description, int source);
    void execute(int jobId, int source);

    bool checkComputationLimits(int jobId);

    bool isRequestObsolete(const JobRequest& req);
    bool isAdoptionOfferObsolete(const JobRequest& req, bool alreadyAccepted = false);

    void commit(JobRequest& req);
    bool hasCommitment() const;
    bool hasCommitment(int jobId) const;
    const JobRequest& getCommitment(int jobId);
    void uncommit(int jobId);

    enum JobRequestMode {TARGETED_REJOIN, NORMAL, IGNORE_FAIL};
    enum AdoptionResult {ADOPT_FROM_IDLE, ADOPT_REPLACE_CURRENT, REJECT, DEFER, DISCARD};
    AdoptionResult tryAdopt(const JobRequest& req, JobRequestMode mode, int sender, int& removedJob);
    
    void reactivate(const JobRequest& req, int source);
    void suspend(int jobId);
    void terminate(int jobId);

    void forgetOldJobs();
    void forget(int jobId);
    void free(int jobId);

    std::vector<std::pair<JobRequest, int>> getDeferredRequestsToForward(float time);

    void preregisterJobInBalancer(int jobId);
    void setBalancerVolumeUpdateCallback(std::function<void(int, int, float)> cb) {_balancer->setVolumeUpdateCallback(cb);}
    void advanceBalancing() {_balancer->advance();}
    void handleBalancingMessage(MessageHandle& handle) {_balancer->handle(handle);}
    int getGlobalBalancingEpoch() const {return _balancer->getGlobalEpoch();}
    
    bool hasVolume(int jobId) const {return _balancer->hasVolume(jobId);}
    int getVolume(int jobId) const {return _balancer->getVolume(jobId);}
    void handleDemandUpdate(Job& job, int demand) {_balancer->onDemandChange(job, demand);}

    bool has(int id) const;
    Job& get(int id) const;
    Job& getActive();
    
    int getLoad() const;
    void setLoad(int load, int whichJobId);
    bool isIdle() const;
    bool hasDormantRoot() const;
    bool hasDormantJob(int id) const;
    std::vector<int> getDormantJobs() const;

    bool hasPendingRootReactivationRequest() const;
    JobRequest loadPendingRootReactivationRequest();
    void setPendingRootReactivationRequest(JobRequest&& req);

    std::string toStr(int j, int idx) const;
    
};

#endif