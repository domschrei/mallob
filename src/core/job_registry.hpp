
#pragma once

#include <queue>

#include "app/app_message_subscription.hpp"
#include "util/logger.hpp"
#include "util/robin_hood.hpp"
#include "app/job.hpp"
#include "app/app_registry.hpp"
#include "job_garbage_collector.hpp"
#include "latency_report.hpp"

class JobRegistry {

private:
    Parameters& _params;
    MPI_Comm& _comm;
    JobGarbageCollector _job_gc;

    AppMessageTable _app_msg_table;

    robin_hood::unordered_map<int, Job*> _jobs;
    bool _has_commitment {false};
    int _load {0};
    Job* _current_job {nullptr};
    robin_hood::unordered_map<int, int> _num_reactivators_per_job;

    float _time_of_last_adoption = 0;
    float _total_busy_time = 0;
    LatencyReport _latency_report;

    bool _memory_panic {false};

public:
    JobRegistry(Parameters& params, MPI_Comm& comm) : _params(params), _comm(comm) {}

    Job& create(int jobId, int applicationId, bool incremental) {

        Job::JobSetup setup;
        setup.commSize = MyMpi::size(_comm);
        setup.worldRank = MyMpi::rank(MPI_COMM_WORLD);
        setup.jobId = jobId;
        setup.applicationId = applicationId;
        setup.incremental = incremental;

        _jobs[jobId] = app_registry::getJobCreator(applicationId)(_params, setup, _app_msg_table);
        _job_gc.numStoredJobs()++;
        return *_jobs[jobId];
    }

    const robin_hood::unordered_map<int, Job*>& getJobMap() const {
        return _jobs;
    }

    void processAppMessage(int source, int mpiTag, JobMessage& msg) {

        LOG(V5_DEBG, "APPMSG RECV %lu <~ %lu [%i]\n", msg.contextIdOfDestination, msg.contextIdOfSender, source);

        auto it = _app_msg_table.find(msg.contextIdOfDestination);
        if (it == _app_msg_table.end()) {
            LOG(V1_WARN, "[WARN] Job message for unregistered job #%i\n", msg.jobId);
            msg.returnToSender(source, mpiTag);
            return;
        }
        AppMessageListener& l = *it->second;
        l.communicate(source, mpiTag, msg);
    }

    void setCommitted() {
        _has_commitment = true;
    }
    void unsetCommitted() {
        _has_commitment = false;
    }
    bool committed() const {
        return _has_commitment;
    }

    bool has(int id) const {
        return _jobs.count(id) > 0;
    }
    Job& get(int id) const {
        assert(_jobs.count(id));
        return *_jobs.at(id);
    }

    bool isBusyOrCommitted() const {
        return hasActiveJob() || committed();
    }
    bool hasActiveJob() const {
        return _load == 1;
    }
    Job& getActive() {
        return *_current_job;
    }

    bool hasCommitment(int jobId) const {
        return has(jobId) && get(jobId).hasCommitment();
    }
    const JobRequest& getCommitment(int jobId) {
        return get(jobId).getCommitment();
    }

    void incrementNumReactivators(int jobId) {
        _num_reactivators_per_job[jobId]++;
    }
    void decrementNumReactivators(int jobId) {
        _num_reactivators_per_job[jobId]--;
    }
    int getNumReactivators(int jobId) {
        return _num_reactivators_per_job[jobId];
    }

    void setLoad(int load, int whichJobId) {
        assert(load + _load == 1); // (load WAS 1) XOR (load BECOMES 1)
        _load = load;
        assert(has(whichJobId));
        if (load == 1) {
            assert(_current_job == NULL);
            LOG(V3_VERB, "LOAD 1 (+%s)\n", get(whichJobId).toStr());
            _current_job = &get(whichJobId);
            _time_of_last_adoption = Timer::elapsedSecondsCached();
        }
        if (load == 0) {
            assert(_current_job != NULL);
            LOG(V3_VERB, "LOAD 0 (-%s)\n", get(whichJobId).toStr());
            _current_job = NULL;
            float timeBusy = Timer::elapsedSecondsCached() - _time_of_last_adoption;
            _total_busy_time += timeBusy;
        }
    }

    int getLoad() const {
        return _load;
    }

    bool hasDormantRoot() const {
        for (auto& [_, job] : _jobs) {
            if (job->getJobTree().isRoot() && job->getState() == SUSPENDED) 
                return true;
        }
        return false;
    }

    bool hasDormantJob(int jobId) const {
        return has(jobId) && (get(jobId).getState() == SUSPENDED);
    }

    std::vector<int> getDormantJobs() const {
        std::vector<int> out;
        for (auto& [jobId, _] : _jobs) {
            if (hasDormantJob(jobId)) out.push_back(jobId);
        }
        return out;
    }

    bool hasInactiveJobsWaitingForReactivation() const {
        if (!_params.reactivationScheduling()) return false;
        for (auto& [_, job] : _jobs) {
            if (job->getState() == SUSPENDED && job->getJobTree().isWaitingForReactivation()) 
                return true;
        }
        return false;
    }

    void setMemoryPanic(bool panic) {
        _memory_panic = panic;
    }

    void checkOldJobs() {
        // Find "forgotten" jobs in destruction queue which can now be destructed.
        // May also be required to advance the destructibility of a job.
        _job_gc.forgetOldJobs();
    }

    std::vector<int> findJobsToForget() {

        std::vector<int> jobsToForget;
        int jobCacheSize = _params.jobCacheSize();
        size_t numJobsWithDescription = 0;

        // Scan jobs for being forgettable
        struct SuspendedJobComparator {
            bool operator()(const std::pair<int, float>& left, const std::pair<int, float>& right) {
                return left.second < right.second;
            };
        };
        std::priority_queue<std::pair<int, float>, std::vector<std::pair<int, float>>, SuspendedJobComparator> suspendedQueue;
        for (auto [id, jobPtr] : _jobs) {
            Job& job = *jobPtr;
            if (job.hasDescription()) numJobsWithDescription++;
            if (job.hasCommitment()) continue;
            // Old inactive job
            if (job.getState() == INACTIVE && job.getAge() >= 10) {
                jobsToForget.push_back(id);
                continue;
            }
            // Suspended job: Forget w.r.t. age, but only if there is a limit on the job cache
            if (job.getState() == SUSPENDED && getNumReactivators(id) == 0 
                        && (_memory_panic || jobCacheSize > 0)) {
                // Job must not be rooted here
                if (job.getJobTree().isRoot()) continue;
                // Insert job into PQ according to its age
                float age = job.getAgeSinceActivation();
                suspendedQueue.emplace(id, age);
            }
        }

        // Mark jobs as forgettable as long as job cache is exceeded
        // (mark ALL eligible jobs if memory panic is triggered)
        while ((!suspendedQueue.empty() && _memory_panic) || (int)suspendedQueue.size() > jobCacheSize) {
            jobsToForget.push_back(suspendedQueue.top().first);
            suspendedQueue.pop();
        }

        if (!_jobs.empty())
            LOG(V4_VVER, "contexts=%i descriptions=%i\n", _jobs.size(), numJobsWithDescription);
        
        return jobsToForget;
    }

    void erase(Job* jobPtr) {
        int jobId = jobPtr->getId();
        _latency_report.report(*jobPtr);
        _job_gc.orderDeletion(jobPtr);
        _jobs.erase(jobId);
    }

    std::vector<int> collectAllJobs() {
        std::vector<int> jobIds;
        for (auto idJobPair : _jobs) jobIds.push_back(idJobPair.first);
        return jobIds;
    }

    bool hasJobsLeftToDelete() const {
        return _job_gc.hasJobsLeftInDestructQueue();
    }

    ~JobRegistry() {
        // Output total busy time
        LOG(V3_VERB, "busytime=%.3f\n", _total_busy_time);
    }
};
