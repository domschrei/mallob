
#pragma once

#include <list>

#include "util/hashing.hpp"
#include "app/job.hpp"
#include "data/job_transfer.hpp"
#include "data/worker_sysstate.hpp"
#include "comm/msg_queue/message_subscription.hpp"
#include "reactivation_scheduler.hpp"
#include "request_manager.hpp"
#include "result_store.hpp"
#include "job_description_interface.hpp"
#include "balancing/event_driven_balancer.hpp"

// forward declarations
class RandomizedRoutingTree;
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
    WorkerSysState& _sys_state;
    JobRegistry& _job_registry;

    std::unique_ptr<RequestMatcher> _req_matcher;
    RequestManager _req_mgr;
    EventDrivenBalancer _balancer;

    JobDescriptionInterface _desc_interface;
    ReactivationScheduler _reactivation_scheduler;
    ResultStore _result_store;

    std::list<MessageSubscription> _subscriptions;

public:
    SchedulingManager(Parameters& params, MPI_Comm& comm, RandomizedRoutingTree& routingTree, 
        JobRegistry& jobRegistry, WorkerSysState& sysstate);
    ~SchedulingManager();

    RequestMatcher* createRequestMatcher();

    void checkActiveJob();
    void advanceBalancing();
    bool checkComputationLimits(int jobId);
    void forwardDeferredRequests() {_req_mgr.forwardDeferredRequests();}
    void tryAdoptPendingRootActivationRequest();
    void forgetOldJobs();
    void triggerMemoryPanic();
    
    enum JobRequestMode {TARGETED_REJOIN, NORMAL, IGNORE_FAIL};
    void handleIncomingJobRequest(MessageHandle& handle, JobRequestMode mode);

    int getGlobalBalancingEpoch() const;

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

    void leaveJobTree(Job& job, bool notifyParent);
    void initiateVolumeUpdate(Job& job);
    void updateVolume(int jobId, int volume, int balancingEpoch, float eventLatency);
    void propagateVolumeUpdate(Job& job, int volume, int balancingEpoch);

    void commit(Job& job, JobRequest& req);
    void uncommit(Job& job, bool leaving);
    void execute(Job& job, int source);
    void resume(Job& job, const JobRequest& req, int source);
    void suspend(Job& job);
    void terminate(Job& job);
    void eraseJobAndQueueForDeletion(Job& job);

    void preregisterJobInBalancer(Job& job);
    void unregisterJobFromBalancer(Job& job);

    bool has(int id) const;
    Job& get(int id) const;
    void setLoad(int load, int jobId);
        
    void handleDemandUpdate(Job& job, int demand);
    void interruptJob(int jobId, bool terminate, bool reckless);

    bool isRequestObsolete(const JobRequest& req);
    enum AdoptionResult {ADOPT, REJECT, DEFER, DISCARD};
    AdoptionResult tryAdopt(JobRequest& req, JobRequestMode mode, int sender);
    bool isAdoptionOfferObsolete(const JobRequest& req, bool alreadyAccepted = false);
};
