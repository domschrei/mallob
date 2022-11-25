
#pragma once

#include <list>

#include "util/hashing.hpp"
#include "app/job.hpp"
#include "data/job_transfer.hpp"
#include "balancing/event_driven_balancer.hpp"
#include "util/sys/background_worker.hpp"
#include "balancing/request_matcher.hpp"
#include "data/worker_sysstate.hpp"
#include "scheduling/local_scheduler.hpp"
#include "comm/message_subscription.hpp"
#include "job_registry.hpp"
#include "reactivation_scheduler.hpp"
#include "request_cache.hpp"

class SchedulingManager {

private:
    Parameters& _params;
    float _wcsecs_per_instance;
    float _cpusecs_per_instance;
    float _load_factor;
    float _balance_period;

    MPI_Comm& _comm;
    std::unique_ptr<EventDrivenBalancer> _balancer;
    std::shared_ptr<RequestMatcher> _req_matcher;
    JobRegistry& _job_registry;
    ReactivationScheduler _reactivation_scheduler;
    RequestCache _req_cache;

    float _last_balancing_initiation;

    WorkerSysState& _sys_state;

    std::list<std::vector<float>> _desire_latencies;

    DeflectJobRequestCallback _cb_deflect_job_request;

    robin_hood::unordered_map<int, int> _send_id_to_job_id;
    robin_hood::unordered_map<std::pair<int, int>, JobResult, IntPairHasher> _pending_results;

    std::list<MessageSubscription> _subscriptions;

public:
    SchedulingManager(Parameters& params, MPI_Comm& comm, JobRegistry& jobRegistry, WorkerSysState& sysstate);
    ~SchedulingManager();
    
    // Initialization methods
    void setRequestMatcher(std::shared_ptr<RequestMatcher>& reqMatcher) {_req_matcher = reqMatcher;}
    void setCallbackToDeflectJobRequest(DeflectJobRequestCallback cb) {
        _cb_deflect_job_request = cb;
    }

    void checkActiveJob();
    bool checkComputationLimits(int jobId);
    void advanceBalancing(float time) {_balancer->advance(time);}
    void forwardDeferredRequests() {_req_cache.forwardDeferredRequests(_cb_deflect_job_request);}
    void tryAdoptPendingRootActivationRequest();
    void forgetOldJobs();
    void triggerMemoryPanic();
    
    enum JobRequestMode {TARGETED_REJOIN, NORMAL, IGNORE_FAIL};
    void handleIncomingJobRequest(MessageHandle& handle, JobRequestMode mode);

    int getGlobalBalancingEpoch() const {return _balancer->getGlobalEpoch();}

    std::string toStr(int j, int idx) const;

private:
    
    void handleAdoptionOffer(MessageHandle& handle);
    void handleRejectionOfDirectedRequest(MessageHandle& handle);
    void handleAnswerToAdoptionOffer(MessageHandle& handle);
    void handleQueryForJobDescription(MessageHandle& handle);
    void handleIncomingJobDescription(MessageHandle& handle);
    void handleJobDescriptionSent(int sendId);
    void handleQueryForExplicitVolumeUpdate(MessageHandle& handle);
    void handleExplicitVolumeUpdate(MessageHandle& handle);
    void handleLeavingChild(MessageHandle& handle);
    void handleJobInterruption(MessageHandle& handle);
    void handleIncrementalJobFinished(MessageHandle& handle);
    void handleApplicationMessage(MessageHandle& handle);
    void handleQueryForJobResult(MessageHandle& handle);
    void handleObsoleteJobResult(MessageHandle& handle);
    void handleJobResultFound(MessageHandle& handle);
    void handleJobReleasedFromWaitingForReactivation(MessageHandle& handle);

    void sendJobDescription(int jobId, int revision, int dest);
    bool appendRevision(int jobId, const std::shared_ptr<std::vector<uint8_t>>& description, int source);
    void initiateVolumeUpdate(int jobId);
    void updateVolume(int jobId, int volume, int balancingEpoch, float eventLatency);
    void propagateVolumeUpdate(Job& job, int volume, int balancingEpoch);

    void commit(JobRequest& req);
    void uncommit(int jobId);
    void execute(int jobId, int source);
    void reactivate(const JobRequest& req, int source);
    void suspend(int jobId);
    void terminate(int jobId);
    void eraseJobAndQueueForDeletion(int jobId);

    void setMemoryPanic(bool panic) {_job_registry.setMemoryPanic(panic);}
    void preregisterJobInBalancer(int jobId);
    void handleBalancingMessage(MessageHandle& handle) {_balancer->handle(handle);}
    void unregisterJobFromBalancer(int jobId) {_balancer->onTerminate(get(jobId));}

    bool has(int id) const;
    Job& get(int id) const;
    void setLoad(int load, int jobId);
        
    void handleDemandUpdate(Job& job, int demand) {
        _balancer->onDemandChange(job, demand);
        job.setLastDemand(demand);
    }
    void interruptJob(int jobId, bool terminate, bool reckless);

    bool isRequestObsolete(const JobRequest& req);
    enum AdoptionResult {ADOPT_FROM_IDLE, ADOPT_REPLACE_CURRENT, REJECT, DEFER, DISCARD};
    AdoptionResult tryAdopt(const JobRequest& req, JobRequestMode mode, int sender, int& removedJob);
    bool isAdoptionOfferObsolete(const JobRequest& req, bool alreadyAccepted = false);
       
    bool hasVolume(int jobId) const {return _balancer->hasVolume(jobId);}
    int getVolume(int jobId) const {return _balancer->getVolume(jobId);}
    void spawnJobRequest(int jobId, bool left, int balancingEpoch);
    void emitJobRequest(const JobRequest& req, int tag, bool left, int dest);
    void activateRootRequest(int jobId);
};
