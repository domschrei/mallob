
#pragma once

#include <list>

#include "util/hashing.hpp"
#include "app/job.hpp"
#include "data/job_transfer.hpp"
#include "data/worker_sysstate.hpp"
#include "comm/message_subscription.hpp"
#include "reactivation_scheduler.hpp"
#include "request_cache.hpp"
#include "result_store.hpp"
#include "job_description_interface.hpp"

// forward declarations
class RandomizedRoutingTree;
class EventDrivenBalancer;
class RequestMatcher;
class JobRegistry;

class SchedulingManager {

private:
    Parameters& _params;
    float _wcsecs_per_instance;
    float _cpusecs_per_instance;
    float _load_factor;
    float _balance_period;
    MPI_Comm& _comm;

    RandomizedRoutingTree& _routing_tree;
    RequestCache _req_cache;
    std::unique_ptr<EventDrivenBalancer> _balancer;
    std::shared_ptr<RequestMatcher> _req_matcher;
    JobRegistry& _job_registry;
    JobDescriptionInterface _desc_interface;
    ReactivationScheduler _reactivation_scheduler;
    ResultStore _result_store;
    WorkerSysState& _sys_state;

    std::list<MessageSubscription> _subscriptions;

public:
    SchedulingManager(Parameters& params, MPI_Comm& comm, RandomizedRoutingTree& routingTree, 
        JobRegistry& jobRegistry, WorkerSysState& sysstate);
    ~SchedulingManager();
    
    // Initialization methods
    void setRequestMatcher(std::shared_ptr<RequestMatcher>& reqMatcher) {_req_matcher = reqMatcher;}

    void checkActiveJob();
    void advanceBalancing(float time);
    bool checkComputationLimits(int jobId);
    //void advanceBalancing(float time) {_balancer->advance(time);}
    void forwardDeferredRequests() {_req_cache.forwardDeferredRequests();}
    void tryAdoptPendingRootActivationRequest();
    void forgetOldJobs();
    void triggerMemoryPanic();
    
    enum JobRequestMode {TARGETED_REJOIN, NORMAL, IGNORE_FAIL};
    void handleIncomingJobRequest(MessageHandle& handle, JobRequestMode mode);

    int getGlobalBalancingEpoch() const;

    std::string toStr(int j, int idx) const;

private:
    void handleAdoptionOffer(MessageHandle& handle);
    void handleRejectionOfDirectedRequest(MessageHandle& handle);
    void handleAnswerToAdoptionOffer(MessageHandle& handle);
    void handleIncomingJobDescription(MessageHandle& handle);
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

    void initiateVolumeUpdate(Job& job);
    void updateVolume(int jobId, int volume, int balancingEpoch, float eventLatency);
    void propagateVolumeUpdate(Job& job, int volume, int balancingEpoch);
    void deflectJobRequest(JobRequest& request, int senderRank);

    void commit(JobRequest& req);
    void uncommit(int jobId);
    void execute(Job& job, int source);
    void reactivate(const JobRequest& req, int source);
    void suspend(int jobId);
    void terminate(int jobId);
    void eraseJobAndQueueForDeletion(int jobId);

    void preregisterJobInBalancer(Job& job);
    void unregisterJobFromBalancer(int jobId);

    bool has(int id) const;
    Job& get(int id) const;
    void setLoad(int load, int jobId);
        
    void handleDemandUpdate(Job& job, int demand);
    void interruptJob(int jobId, bool terminate, bool reckless);

    bool isRequestObsolete(const JobRequest& req);
    enum AdoptionResult {ADOPT_FROM_IDLE, ADOPT_REPLACE_CURRENT, REJECT, DEFER, DISCARD};
    AdoptionResult tryAdopt(const JobRequest& req, JobRequestMode mode, int sender, int& removedJob);
    bool isAdoptionOfferObsolete(const JobRequest& req, bool alreadyAccepted = false);

    void spawnJobRequest(int jobId, bool left, int balancingEpoch);
    void emitJobRequest(const JobRequest& req, int tag, bool left, int dest);
    void activateRootRequest(int jobId);
};
