
#include "job_database.hpp"

#include "util/assert.hpp"
#include <algorithm>
#include <queue>
#include <utility>
#include <climits>

#include "app/sat/forked_sat_job.hpp"
#include "app/sat/threaded_sat_job.hpp"
#include "app/dummy/dummy_job.hpp"

#include "util/sys/timer.hpp"
#include "util/logger.hpp"
#include "balancing/event_driven_balancer.hpp"
#include "util/sys/watchdog.hpp"
#include "util/sys/proc.hpp"
#include "util/data_statistics.hpp"

JobDatabase::JobDatabase(Parameters& params, MPI_Comm& comm, WorkerSysState& sysstate):
        _params(params), _comm(comm), _sys_state(sysstate) {
    _wcsecs_per_instance = params.jobWallclockLimit();
    _cpusecs_per_instance = params.jobCpuLimit();
    _load = 0;
    _last_balancing_initiation = 0;
    _current_job = NULL;
    _load_factor = params.loadFactor();
    assert(0 < _load_factor && _load_factor <= 1.0);
    _balance_period = params.balancingPeriod();       

    // Initialize balancer
    _balancer = std::unique_ptr<EventDrivenBalancer>(new EventDrivenBalancer(_comm, _params));
    // Initialize janitor (cleaning up old jobs)
    _janitor.run([this]() {runJanitor();});
}

Job& JobDatabase::createJob(int commSize, int worldRank, int jobId, JobDescription::Application application) {

    switch (application) {
    case JobDescription::Application::ONESHOT_SAT:
    case JobDescription::Application::INCREMENTAL_SAT:
        if (_params.applicationSpawnMode() == "fork") {
            _jobs[jobId] = new ForkedSatJob(_params, commSize, worldRank, jobId, application);
        } else {
            _jobs[jobId] = new ThreadedSatJob(_params, commSize, worldRank, jobId, application);
        }
        break;
    case JobDescription::Application::DUMMY:
        _jobs[jobId] = new DummyJob(_params, commSize, worldRank, jobId);
    }
    _num_stored_jobs++;
    return *_jobs[jobId];
}

bool JobDatabase::appendRevision(int jobId, const std::shared_ptr<std::vector<uint8_t>>& description, int source) {

    if (!has(jobId)) {
        LOG(V1_WARN, "[WARN] Unknown job #%i : discard desc. of size %i\n", jobId, description->size());
        return false;
    }
    auto& job = get(jobId);
    int rev = JobDescription::readRevisionIndex(*description);
    if (job.hasDescription()) {
        if (rev != job.getMaxConsecutiveRevision()+1) {
            // Revision data would cause a "hole" in the list of job revision data
            LOG(V1_WARN, "[WARN] #%i rev. %i inconsistent w/ max. consecutive rev. %i : discard desc. of size %i\n", 
                jobId, rev, job.getMaxConsecutiveRevision(), description->size());
            return false;
        }
    } else if (rev != 0) {
        LOG(V1_WARN, "[WARN] #%i invalid \"first\" rev. %i : discard desc. of size %i\n", jobId, rev, description->size());
            return false;
    }

    // Push revision description
    job.pushRevision(description);
    return true;
}

void JobDatabase::execute(int jobId, int source) {

    if (!has(jobId)) {
        LOG(V1_WARN, "[WARN] Unknown job #%i\n", jobId);
        return;
    }
    auto& job = get(jobId);

    // Execute job
    setLoad(1, jobId);
    LOG_ADD_SRC(V3_VERB, "EXECUTE %s", source, job.toStr());
    if (job.getState() == INACTIVE) {
        // Execute job for the first time
        job.start();
    } else {
        // Restart job
        job.resume();
    }

    int demand = job.getDemand();
    _balancer->onActivate(job, demand);
    job.setLastDemand(demand);
}

void JobDatabase::preregisterJobInBalancer(int jobId) {
    assert(has(jobId));
    auto& job = get(jobId);
    int demand = std::max(1, job.getJobTree().isRoot() ? 0 : job.getDemand());
    _balancer->onActivate(job, demand);
    if (job.getJobTree().isRoot()) job.setLastDemand(demand);
}

bool JobDatabase::checkComputationLimits(int jobId) {

    auto& job = get(jobId);
    if (!job.getJobTree().isRoot()) return false;
    return job.checkResourceLimit(_wcsecs_per_instance, _cpusecs_per_instance);
}

bool JobDatabase::isRequestObsolete(const JobRequest& req) {

    // Requests for a job root never become obsolete
    if (req.requestedNodeIndex == 0) return false;

    if (req.balancingEpoch < getGlobalBalancingEpoch()) {
        // Request from a past balancing epoch
        LOG(V4_VVER, "%s : past epoch\n", req.toStr().c_str());
        return true;
    }

    if (!has(req.jobId)) return false;

    Job& job = get(req.jobId);
    if (job.getState() == ACTIVE) {
        // Does this node KNOW that the request is already completed?
        if (req.requestedNodeIndex == job.getIndex()
        || (job.getJobTree().hasLeftChild() && req.requestedNodeIndex == job.getJobTree().getLeftChildIndex())
        || (job.getJobTree().hasRightChild() && req.requestedNodeIndex == job.getJobTree().getRightChildIndex())) {
            // Request already completed!
            LOG(V4_VVER, "%s : already completed\n", req.toStr().c_str());
            return true;
        }
    }
    return false;
}

bool JobDatabase::isAdoptionOfferObsolete(const JobRequest& req, bool alreadyAccepted) {

    // Requests for a job root never become obsolete
    if (req.requestedNodeIndex == 0) return false;

    if (!has(req.jobId)) {
        // Job not known anymore: obsolete
        LOG(V4_VVER, "%s : job unknown\n", req.toStr().c_str());
        return true;
    }

    Job& job = get(req.jobId);
    if (job.getState() != ACTIVE && !hasCommitment(req.jobId)) {
        // Job is not active
        LOG(V4_VVER, "%s : job inactive (%s)\n", req.toStr().c_str(), job.jobStateToStr());
        return true;
    }
    if (req.requestedNodeIndex != job.getJobTree().getLeftChildIndex() 
            && req.requestedNodeIndex != job.getJobTree().getRightChildIndex()) {
        // Requested node index is not a valid child index for this job
        LOG(V4_VVER, "%s : not a valid child index (any more)\n", job.toStr());
        return true;
    }
    if (req.revision < job.getRevision()) {
        // Job was updated in the meantime
        LOG(V4_VVER, "%s : rev. %i not up to date\n", req.toStr().c_str(), req.revision);
        return true;
    }
    if (alreadyAccepted) {
        return false;
    }
    if (req.requestedNodeIndex == job.getJobTree().getLeftChildIndex() && job.getJobTree().hasLeftChild()) {
        // Job already has a left child
        LOG(V4_VVER, "%s : already has left child\n", req.toStr().c_str());
        return true;

    }
    if (req.requestedNodeIndex == job.getJobTree().getRightChildIndex() && job.getJobTree().hasRightChild()) {
        // Job already has a right child
        LOG(V4_VVER, "%s : already has right child\n", req.toStr().c_str());
        return true;
    }
    return false;
}

void JobDatabase::commit(JobRequest& req) {
    if (has(req.jobId)) {
        Job& job = get(req.jobId);
        LOG(V3_VERB, "COMMIT %s -> #%i:%i\n", job.toStr(), req.jobId, req.requestedNodeIndex);
        job.commit(req);
        _has_commitment = true;
        
        // Subscribe for volume updates for this job even if the job is not active yet
        // Also reserves a PE of space for this job in case this is a root node
        preregisterJobInBalancer(req.jobId);

        if (_coll_assign) _coll_assign->setStatusDirty();
    }
}

void JobDatabase::initScheduler(JobRequest& req, std::function<void(const JobRequest& req, int tag, bool left, int dest)> emitJobReq) {

    auto& job = get(req.jobId);
    auto key = std::pair<int, int>(req.jobId, req.requestedNodeIndex);
    if (!_schedulers.count(key)) {
        _schedulers.emplace(key, 
            job.constructScheduler([this, emitJobReq](const JobRequest& req, int tag, bool left, int dest) {
                emitJobReq(req, tag, left, dest);
            })
        );
        _num_schedulers_per_job[req.jobId]++;
    }

    if (job.getJobTree().isRoot()) {
        if (req.revision == 0) {
            // Initialize the root's local scheduler, which in turn initializes
            // the scheduler of each child node (recursively)
            JobSchedulingUpdate update;
            update.epoch = req.balancingEpoch;
            if (update.epoch < 0) update.epoch = 0;
            _schedulers.at(key).initializeScheduling(update, job.getJobTree().getParentNodeRank());
        } else {
            _schedulers.at(key).beginResumptionAsRoot();
        }
    } else {
        assert(_schedulers.at(key).canCommit());
        _schedulers.at(key).resetRole();
    }
}

void JobDatabase::suspendScheduler(Job& job) {
    auto key = std::pair<int, int>(job.getId(), job.getIndex());
    if (_schedulers.count(key)) {
        auto& sched = _schedulers.at(key);
        if (sched.isActive()) sched.beginSuspension();
        if (job.getIndex() > 0 && sched.canCommit()) {
            _schedulers.erase(key);
            _num_schedulers_per_job[job.getId()]--;
        }
    }
}

bool JobDatabase::hasCommitment() const {
    return _has_commitment;
}

bool JobDatabase::hasCommitment(int jobId) const {
    return has(jobId) && get(jobId).hasCommitment();
}

const JobRequest& JobDatabase::getCommitment(int jobId) {
    return get(jobId).getCommitment();
}

void JobDatabase::uncommit(int jobId) {
    if (has(jobId)) {
        LOG(V3_VERB, "UNCOMMIT %s\n", get(jobId).toStr());
        get(jobId).uncommit();
        _has_commitment = false;
        if (_coll_assign) _coll_assign->setStatusDirty();
    }
}

JobDatabase::AdoptionResult JobDatabase::tryAdopt(const JobRequest& req, JobRequestMode mode, int sender, int& removedJob) {

    // Decide whether job should be adopted or bounced to another node
    removedJob = -1;
    
    // Already have another commitment?
    if (_has_commitment) {
        if (_coll_assign) _coll_assign->setStatusDirty();
        return REJECT;
    }

    // Does this node have a dormant root which is NOT this job?
    if (hasDormantRoot() && (
        !has(req.jobId)
        || !get(req.jobId).getJobTree().isRoot() 
        || get(req.jobId).getState() != SUSPENDED
    )) {
        LOG(V4_VVER, "Reject %s : dormant root present\n", req.toStr().c_str());
        return REJECT;
    }

    if (hasScheduler(req.jobId, req.requestedNodeIndex) && !getScheduler(req.jobId, req.requestedNodeIndex).canCommit()) {
        LOG(V1_WARN, "%s : still have an active scheduler of this node!\n", req.toStr().c_str());
        return REJECT;
    }

    bool isThisDormantRoot = has(req.jobId) && get(req.jobId).getJobTree().isRoot();
    if (isThisDormantRoot) {
        if (req.requestedNodeIndex > 0) {
            // Explicitly avoid to adopt a non-root node of the job of which I have a dormant root
            // (commit would overwrite job index!)
            if (_coll_assign) _coll_assign->setStatusDirty();
            return REJECT;
        }
    } else {
        if (hasDormantRoot() && req.requestedNodeIndex == 0) {
            // Cannot adopt a root node while there is still another dormant root here
            if (_coll_assign) _coll_assign->setStatusDirty();
            return REJECT;
        }
    }

    // Is node idle and not committed to another job?
    if (!isBusyOrCommitted()) {
        if (mode != TARGETED_REJOIN) return ADOPT_FROM_IDLE;
        // Oneshot request: Job must be present and suspended
        else if (hasDormantJob(req.jobId)) {
            return ADOPT_FROM_IDLE;
        } else {
            if (_coll_assign) _coll_assign->setStatusDirty();
            return REJECT;
        }
    }
    // -- node is busy in some form

    // Request for a root node:
    // Possibly adopt the job while dismissing the active job
    if (req.requestedNodeIndex == 0 && !_params.reactivationScheduling()) {

        // Adoption only works if this node does not yet compute for that job
        if (!has(req.jobId) || get(req.jobId).getState() != ACTIVE) {

            // Current job must be a non-root leaf node
            Job& job = getActive();
            if (job.getState() == ACTIVE && !job.getJobTree().isRoot() && job.getJobTree().isLeaf()) {
                
                // Inform parent node of the original job  
                LOG(V4_VVER, "Suspend %s ...\n", job.toStr());
                LOG(V4_VVER, "... to adopt starving %s\n", 
                                toStr(req.jobId, req.requestedNodeIndex).c_str());

                removedJob = job.getId();
                suspend(removedJob);
                return ADOPT_REPLACE_CURRENT;
            }
        }

        // Adoption did not work out: Defer the request if a certain #hops is reached
        if (req.numHops > 0 && req.numHops % std::max(32, MyMpi::size(_comm)) == 0) {
            LOG(V3_VERB, "Defer %s\n", req.toStr().c_str());
            _deferred_requests.emplace_back(Timer::elapsedSeconds(), sender, req);
            return DEFER;
        }
    }

    if (_coll_assign) _coll_assign->setStatusDirty();
    return REJECT;
}

void JobDatabase::reactivate(const JobRequest& req, int source) {
    // Already has job description: Directly resume job (if not terminated yet)
    assert(has(req.jobId));
    Job& job = get(req.jobId);
    job.updateJobTree(req.requestedNodeIndex, req.rootRank, req.requestingNodeRank);
    setLoad(1, req.jobId);
    assert(!hasCommitment(req.jobId));
    LOG_ADD_SRC(V3_VERB, "RESUME %s", source, 
                toStr(req.jobId, req.requestedNodeIndex).c_str());
    job.resume();

    int demand = job.getDemand();
    _balancer->onActivate(job, demand);
    job.setLastDemand(demand);
}

void JobDatabase::suspend(int jobId) {
    assert(has(jobId) && get(jobId).getState() == ACTIVE);
    Job& job = get(jobId);
    // Suspend (and possibly erase) job scheduler
    suspendScheduler(job);    
    job.suspend();
    setLoad(0, jobId);
    LOG(V3_VERB, "SUSPEND %s\n", job.toStr());
    _balancer->onSuspend(job);
}

void JobDatabase::terminate(int jobId) {
    assert(has(jobId));
    Job& job = get(jobId);
    bool wasTerminatedBefore = job.getState() == JobState::PAST;
    if (hasActiveJob() && getActive().getId() == jobId) {
        setLoad(0, jobId);
    }

    if (!wasTerminatedBefore) {
        // Gather statistics
        auto numDesires = job.getJobTree().getNumDesires();
        auto numFulfilledDesires = job.getJobTree().getNumFulfiledDesires();
        auto sumDesireLatencies = job.getJobTree().getSumOfDesireLatencies();
        float desireFulfilmentRatio = numDesires == 0 ? 0 : (float)numFulfilledDesires / numDesires;
        float meanFulfilmentLatency = numFulfilledDesires == 0 ? 0 : sumDesireLatencies / numFulfilledDesires;

        auto& latencies = job.getJobTree().getDesireLatencies();
        float meanLatency = 0, minLatency = 0, maxLatency = 0, medianLatency = 0;
        if (!latencies.empty()) {
            std::sort(latencies.begin(), latencies.end());
            meanLatency = std::accumulate(latencies.begin(), latencies.end(), 0.0f) / latencies.size();
            minLatency = latencies.front();
            maxLatency = latencies.back();
            medianLatency = latencies[latencies.size()/2];
        }

        LOG(V3_VERB, "%s desires fulfilled=%.4f latency={num:%i min:%.5f med:%.5f avg:%.5f max:%.5f}\n",
            job.toStr(), desireFulfilmentRatio, latencies.size(), minLatency, medianLatency, meanLatency, maxLatency);
    }

    //_sys_state.addLocal(SYSSTATE_NUMDESIRES, numDesires);
    //_sys_state.addLocal(SYSSTATE_NUMFULFILLEDDESIRES, numFulfilledDesires);
    //_sys_state.addLocal(SYSSTATE_SUMDESIRELATENCIES, sumDesireLatencies);

    job.terminate();
    if (job.hasCommitment()) uncommit(jobId);
    if (!wasTerminatedBefore) _balancer->onTerminate(job);

    LOG(V4_VVER, "Delete %s\n", job.toStr());
    forget(jobId);
}

void JobDatabase::forgetOldJobs() {

    // Scan inactive schedulers
    auto it = _schedulers.begin();
    while (it != _schedulers.end()) {
        auto& [id, idx] = it->first;
        if (has(id) && (get(id).getState() == ACTIVE || get(id).hasCommitment())) {
            ++it;
            continue;
        }
        LocalScheduler& scheduler = it->second;
        // Condition for a scheduler to be deleted:
        // Either job is not present (any more) or non-root
        if ((!has(id) || idx > 0) && scheduler.canCommit()) {
            it = _schedulers.erase(it);
            _num_schedulers_per_job[id]--;
        } else ++it;
    }

    // Find "forgotten" jobs in destruction queue which can now be destructed
    for (auto it = _job_destruct_queue.begin(); it != _job_destruct_queue.end(); ) {
        Job* job = *it;
        if (job->isDestructible()) {
            LOG(V4_VVER, "%s : order deletion\n", job->toStr());
            // Move pointer to "free" queue emptied by janitor thread
            {
                auto lock = _janitor_mutex.getLock();
                _jobs_to_free.push_back(job);
            }
            _janitor_cond_var.notify();
            it = _job_destruct_queue.erase(it);
        } else ++it;
    }

    std::vector<int> jobsToForget;
    int jobCacheSize = _params.jobCacheSize();
    size_t numJobsWithDescription = 0;

    // Scan jobs for being forgettable
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
        if (job.getState() == SUSPENDED && _num_schedulers_per_job[id] == 0 && jobCacheSize > 0) {
            // Job must not be rooted here
            if (job.getJobTree().isRoot()) continue;
            // Insert job into PQ according to its age
            float age = job.getAgeSinceActivation();
            suspendedQueue.emplace(id, age);
        }
    }

    // Mark jobs as forgettable as long as job cache is exceeded
    while ((int)suspendedQueue.size() > jobCacheSize) {
        jobsToForget.push_back(suspendedQueue.top().first);
        suspendedQueue.pop();
    }

    if (!_jobs.empty())
        LOG(V4_VVER, "contexts=%i descriptions=%i\n", _jobs.size(), numJobsWithDescription);
    
    // Perform forgetting of jobs
    for (int jobId : jobsToForget) forget(jobId);
}

void JobDatabase::forget(int jobId) {
    Job* jobPtr;
    {
        Job& job = get(jobId);
        LOG(V4_VVER, "FORGET %s\n", job.toStr());
        if (job.getState() != PAST) job.terminate();
        assert(job.getState() == PAST);
        jobPtr = &job;

        if (!job.getJobTree().getDesireLatencies().empty())
            _desire_latencies.push_back(std::move(job.getJobTree().getDesireLatencies()));
    }
    _job_destruct_queue.push_back(jobPtr);
    _jobs.erase(jobId);
}

std::vector<std::pair<JobRequest, int>>  JobDatabase::getDeferredRequestsToForward(float time) {
    
    std::vector<std::pair<JobRequest, int>> result;
    for (auto& [deferredTime, senderRank, req] : _deferred_requests) {
        if (time - deferredTime < 1.0f) break;
        result.emplace_back(std::move(req), senderRank);
        LOG(V3_VERB, "Reactivate deferred %s\n", req.toStr().c_str());
    }

    for (size_t i = 0; i < result.size(); i++) {
        _deferred_requests.pop_front();
    }

    return result;
}

bool JobDatabase::has(int id) const {
    return _jobs.count(id) > 0;
}

Job& JobDatabase::get(int id) const {
    assert(_jobs.count(id));
    return *_jobs.at(id);
}

Job& JobDatabase::getActive() {
    return *_current_job;
}

std::string JobDatabase::toStr(int j, int idx) const {
    return "#" + std::to_string(j) + ":" + std::to_string(idx);
}

int JobDatabase::getLoad() const {
    return _load;
}

void JobDatabase::setLoad(int load, int whichJobId) {
    assert(load + _load == 1); // (load WAS 1) XOR (load BECOMES 1)
    _load = load;
    assert(has(whichJobId));
    if (load == 1) {
        assert(_current_job == NULL);
        LOG(V3_VERB, "LOAD 1 (+%s)\n", get(whichJobId).toStr());
        _current_job = &get(whichJobId);
        _time_of_last_adoption = Timer::elapsedSeconds();
    }
    if (load == 0) {
        assert(_current_job != NULL);
        LOG(V3_VERB, "LOAD 0 (-%s)\n", get(whichJobId).toStr());
        _current_job = NULL;
        float timeBusy = Timer::elapsedSeconds() - _time_of_last_adoption;
        _total_busy_time += timeBusy;
    }
    if (_coll_assign) _coll_assign->setStatusDirty();
}

bool JobDatabase::hasActiveJob() const {
    return _load == 1;
}

bool JobDatabase::isBusyOrCommitted() const {
    return hasActiveJob() || hasCommitment();
}

bool JobDatabase::hasDormantRoot() const {
    for (auto& [_, job] : _jobs) {
        if (job->getJobTree().isRoot() && job->getState() == SUSPENDED) 
            return true;
    }
    return false;
}

bool JobDatabase::hasDormantJob(int jobId) const {
    return has(jobId) && (get(jobId).getState() == SUSPENDED);
}

std::vector<int> JobDatabase::getDormantJobs() const {
    std::vector<int> out;
    for (auto& [jobId, _] : _jobs) {
        if (hasDormantJob(jobId)) out.push_back(jobId);
    }
    return out;
}

bool JobDatabase::hasInactiveJobsWaitingForReactivation() const {
    if (!_params.reactivationScheduling()) return false;
    for (auto& [_, job] : _jobs) {
        if (job->getState() == SUSPENDED && job->getJobTree().isWaitingForReactivation()) 
            return true;
    }
    return false;
}

bool JobDatabase::hasPendingRootReactivationRequest() const {
    return _pending_root_reactivate_request.has_value();
}

JobRequest JobDatabase::loadPendingRootReactivationRequest() {
    assert(hasPendingRootReactivationRequest());
    JobRequest r = _pending_root_reactivate_request.value();
    _pending_root_reactivate_request.reset();
    return r;
}

void JobDatabase::setPendingRootReactivationRequest(JobRequest&& req) {
    assert(!hasPendingRootReactivationRequest() 
        || req.jobId == _pending_root_reactivate_request.value().jobId);
    _pending_root_reactivate_request = std::move(req);
}

void JobDatabase::addFutureRequestMessage(int epoch, MessageHandle&& h) {
    _future_request_msgs[epoch].push_back(std::move(h));
}

std::list<MessageHandle> JobDatabase::getArrivedFutureRequests() {
    int presentEpoch = getGlobalBalancingEpoch();
    std::list<MessageHandle> out;
    auto it = _future_request_msgs.begin();
    while (it != _future_request_msgs.end()) {
        auto& [epoch, msgs] = *it;
        if (epoch <= presentEpoch) {
            // found a candidate message
            assert(!msgs.empty());
            out.splice(out.end(), std::move(msgs));
            it = _future_request_msgs.erase(it);
        } else {
            ++it;
        }
    }
    return out;
}

void JobDatabase::addRootRequest(const JobRequest& req) {
    _root_requests[req.jobId].push_back(req);
    _balancer->onProbe(req.jobId);
}

std::optional<JobRequest> JobDatabase::getRootRequest(int jobId) {
    
    auto it = _root_requests.find(jobId);
    if (it == _root_requests.end()) return std::optional<JobRequest>();

    auto& list = it->second;
    std::optional<JobRequest> optReq = std::optional<JobRequest>(list.front());
    list.pop_front();
    if (list.empty()) _root_requests.erase(jobId);
    return optReq;
}

void JobDatabase::runJanitor() {

    auto lg = Logger::getMainInstance().copy("<Janitor>", ".janitor");
    LOGGER(lg, V3_VERB, "tid=%lu\n", Proc::getTid());
    
    while (_janitor.continueRunning() || _num_stored_jobs > 0) {
    
        std::list<Job*> copy;
        {
            // Try to fetch the current jobs to free
            auto lock = _janitor_mutex.getLock();
            _janitor_cond_var.waitWithLockedMutex(lock, [&]() {
                return !_janitor.continueRunning() || !_jobs_to_free.empty();
            });
            if (!_janitor.continueRunning() && _jobs_to_free.empty() && _num_stored_jobs == 0)
                break;
            
            // Copy jobs to free to local list
            for (Job* job : _jobs_to_free) copy.push_back(job);
            _jobs_to_free.clear();
        }

        LOGGER(lg, V5_DEBG, "Found %i job(s) to delete\n", copy.size());
        
        // Free each job
        for (Job* job : copy) {
            int id = job->getId();
            LOGGER(lg, V4_VVER, "DELETE #%i\n", id);
            delete job;
            Logger::getMainInstance().mergeJobLogs(id);
            _num_stored_jobs--;
        }
    }
}

JobDatabase::~JobDatabase() {

    // Suspend current job (if applicable) to compute last slice of busy time
    if (_load == 1) setLoad(0, getActive().getId());
    // Output total busy time
    LOG(V3_VERB, "busytime=%.3f\n", _total_busy_time);

    // Setup a watchdog to get feedback on hanging destructors
    Watchdog watchdog(/*enabled=*/_params.watchdog(), /*checkIntervMillis=*/200, Timer::elapsedSeconds());
    watchdog.setWarningPeriod(500);
    watchdog.setAbortPeriod(10*1000);

    // Collect all jobs from central job table
    std::vector<int> jobIds;
    for (auto idJobPair : _jobs) jobIds.push_back(idJobPair.first);
    
    // Forget each job, move raw pointer to destruct queue
    for (int jobId : jobIds) {
        forget(jobId);
        watchdog.reset();
    }

    // Empty destruct queue into garbage for janitor to clean up
    while (!_job_destruct_queue.empty()) {
        forgetOldJobs();
        _janitor_cond_var.notify();
        watchdog.reset();
        usleep(10*1000); // 10 milliseconds
    }

    _janitor.stopWithoutWaiting();
    _janitor_cond_var.notify();
    watchdog.stop();
    _janitor.stop();

    DataStatistics stats(std::move(_desire_latencies));
    stats.computeStats();
    LOG(V3_VERB, "STATS treegrowth_latencies num:%ld min:%.6f max:%.6f med:%.6f mean:%.6f\n", 
        stats.num(), stats.min(), stats.max(), stats.median(), stats.mean());
    stats.logFullDataIntoFile(".treegrowth-latencies");
}