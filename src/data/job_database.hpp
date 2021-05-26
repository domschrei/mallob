
#ifndef DOMPASCH_MALLOB_JOB_DATABASE_HPP
#define DOMPASCH_MALLOB_JOB_DATABASE_HPP

#include <thread>
#include <list>

#include "util/robin_hood.hpp"
#include "app/job.hpp"
#include "job_transfer.hpp"
#include "balancing/balancer.hpp"

class JobDatabase {

private:
    Parameters& _params;
    float _wcsecs_per_instance;
    float _cpusecs_per_instance;
    float _load_factor;
    float _balance_period;

    MPI_Comm& _comm;
    std::unique_ptr<Balancer> _balancer;

    std::atomic_int _num_stored_jobs = 0;
    robin_hood::unordered_map<int, Job*> _jobs;
    bool _has_commitment = false;

    int _load;
    Job* _current_job;
    float _last_balancing_initiation;

    std::list<std::tuple<float, int, JobRequest>> _deferred_requests;

    struct SuspendedJobComparator {
        bool operator()(const std::pair<int, float>& left, const std::pair<int, float>& right) {
            return left.second < right.second;
        };
    };

    std::thread _janitor;
    std::list<Job*> _jobs_to_free;
    Mutex _janitor_mutex;
    std::atomic_bool _exiting = false;

public:
    JobDatabase(Parameters& params, MPI_Comm& comm);
    ~JobDatabase();

    Job& createJob(int commSize, int worldRank, int jobId);
    bool init(int jobId, const std::shared_ptr<std::vector<uint8_t>>& description, int source);
    bool restart(int jobId, const std::shared_ptr<std::vector<uint8_t>>& description, int source);

    bool checkComputationLimits(int jobId);

    bool isRequestObsolete(const JobRequest& req);
    bool isAdoptionOfferObsolete(const JobRequest& req, bool alreadyAccepted = false);

    void commit(JobRequest& req);
    bool hasCommitment(int jobId) const;
    const JobRequest& getCommitment(int jobId);
    void uncommit(int jobId);

    enum AdoptionResult {ADOPT_FROM_IDLE, ADOPT_REPLACE_CURRENT, REJECT, DEFER, DISCARD};
    AdoptionResult tryAdopt(const JobRequest& req, bool oneshot, int sender, int& removedJob);
    
    void reactivate(const JobRequest& req, int source);
    void suspend(int jobId);
    void stop(int jobId, bool terminate=false);

    void forgetOldJobs();
    void forget(int jobId);
    void free(int jobId);

    std::vector<std::pair<JobRequest, int>> getDeferredRequestsToForward(float time);

    bool isTimeForRebalancing();
    bool beginBalancing();
    bool continueBalancing();
    bool continueBalancing(MessageHandle& handle);
    void finishBalancing();
    robin_hood::unordered_map<int, int> getBalancingResult();

    bool has(int id) const;
    Job& get(int id) const;
    Job& getActive();
    
    int getLoad() const;
    void setLoad(int load, int whichJobId);
    bool isIdle() const;
    bool hasDormantRoot() const;
    
    std::string toStr(int j, int idx) const;
    
};

#endif