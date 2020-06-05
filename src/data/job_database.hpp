
#ifndef DOMPASCH_MALLOB_JOB_DATABASE_HPP
#define DOMPASCH_MALLOB_JOB_DATABASE_HPP

#include <map>
#include <thread>

#include "app/job.hpp"
#include "job_transfer.hpp"
#include "balancing/balancer.hpp"

class JobDatabase {

private:
    Parameters& _params;
    int _threads_per_job;
    float _wcsecs_per_instance;
    float _cpusecs_per_instance;
    float _load_factor;
    float _balance_period;

    EpochCounter& _epoch_counter;
    MPI_Comm& _comm;
    std::unique_ptr<Balancer> _balancer;

    std::map<int, Job*> _jobs;
    std::map<int, JobRequest> _commitments;
    std::map<int, float> _arrivals;
    std::map<int, float> _cpu_time_used;
    std::map<int, float> _last_limit_check;
    std::map<int, int> _volumes;
    std::map<int, std::thread> _initializer_threads;

    int _load;
    Job* _current_job;
    float _last_load_change;

    struct SuspendedJobComparator {
        bool operator()(const std::pair<int, float>& left, const std::pair<int, float>& right) {
            return left.second < right.second;
        };
    };

public:
    JobDatabase(Parameters& params, EpochCounter& epochCounter, MPI_Comm& comm);
    ~JobDatabase();

    Job& createJob(int commSize, int worldRank, int jobId);
    void init(int jobId, std::shared_ptr<std::vector<uint8_t>> description, int source);
    bool checkComputationLimits(int jobId);

    bool isRequestObsolete(const JobRequest& req);
    bool isAdoptionOfferObsolete(const JobRequest& req);

    void commit(JobRequest& req);
    bool hasCommitments() const;
    bool hasCommitment(int jobId) const;
    JobRequest& getCommitment(int jobId);
    void uncommit(int jobId);

    bool tryAdopt(const JobRequest& req, bool oneshot, int& removedJob);
    
    void forgetOldJobs();
    void forget(int jobId);
    void free(int jobId);

    bool isTimeForRebalancing();
    bool beginBalancing();
    bool continueBalancing();
    bool continueBalancing(MessageHandlePtr& handle);
    void finishBalancing();
    const std::map<int, int>& getBalancingResult();
    void overrideBalancerVolume(int jobId, int volume);

    bool has(int id) const;
    Job& get(int id) const;
    Job& getActive();
    
    int getLoad() const;
    void setLoad(int load, int whichJobId);
    bool isIdle() const;

    bool hasVolume(int jobId);
    int getVolume(int jobId);

    std::string toStr(int j, int idx) const;
    
};

#endif