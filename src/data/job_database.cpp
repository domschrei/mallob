
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

    _janitor.run([this]() {
        log(V3_VERB, "Job-DB Janitor tid=%lu\n", Proc::getTid());
        while (_janitor.continueRunning() || _num_stored_jobs > 0) {
            usleep(1000 * 1000);
            std::list<Job*> copy;
            {
                auto lock = _janitor_mutex.getLock();
                copy = std::move(_jobs_to_free);
                _jobs_to_free.clear();
            }
            for (auto job : copy) {
                int id = job->getId();
                delete job;
                Logger::getMainInstance().mergeJobLogs(id);
                _num_stored_jobs--;
            }
        }
    });
}

Job& JobDatabase::createJob(int commSize, int worldRank, int jobId, JobDescription::Application application) {

    switch (application) {
    case JobDescription::Application::SAT:
        if (_params.applicationSpawnMode() == "fork") {
            _jobs[jobId] = new ForkedSatJob(_params, commSize, worldRank, jobId);
        } else {
            _jobs[jobId] = new ThreadedSatJob(_params, commSize, worldRank, jobId);
        }
        break;
    case JobDescription::Application::DUMMY:
        _jobs[jobId] = new DummyJob(_params, commSize, worldRank, jobId);
    }
    _num_stored_jobs++;
    return *_jobs[jobId];
}

bool JobDatabase::appendRevision(int jobId, const std::shared_ptr<std::vector<uint8_t>>& description, int source) {

    if (!has(jobId) || get(jobId).getState() == PAST) {
        log(V1_WARN, "[WARN] Unknown or past job #%i : discard desc. of size %i\n", jobId, description->size());
        return false;
    }
    auto& job = get(jobId);
    int rev = JobDescription::readRevisionIndex(*description);
    if (job.hasDescription() && job.getRevision() >= rev) {
        // Revision data is already present
        log(V1_WARN, "[WARN] #%i rev. %i already present : discard desc. of size %i\n", jobId, rev, description->size());
        return false;
    }

    // Push revision description
    job.pushRevision(description);
    return true;
}

void JobDatabase::execute(int jobId, int source) {

    if (!has(jobId) || get(jobId).getState() == PAST) {
        log(V1_WARN, "[WARN] Unknown or past job #%i\n", jobId);
        return;
    }
    auto& job = get(jobId);

    // Execute job
    setLoad(1, jobId);
    log(LOG_ADD_SRCRANK | V3_VERB, "EXECUTE %s", source, job.toStr());
    if (job.getState() == INACTIVE) {
        // Execute job for the first time
        job.start();
    } else {
        // Restart job
        job.resume();
    }

    _balancer->onActivate(job);
}

void JobDatabase::preregisterJobInBalancer(int jobId) {
    assert(has(jobId));
    _balancer->onActivate(get(jobId));
}

bool JobDatabase::checkComputationLimits(int jobId) {

    auto& job = get(jobId);
    if (!job.getJobTree().isRoot()) return false;
    return job.checkResourceLimit(_wcsecs_per_instance, _cpusecs_per_instance);
}

bool JobDatabase::isRequestObsolete(const JobRequest& req) {

    // Requests for a job root never become obsolete
    if (req.requestedNodeIndex == 0) return false;

    if (req.balancingEpoch < _balancer->getGlobalEpoch()) {
        // Request from a past balancing epoch
        log(V4_VVER, "%s : past epoch\n", req.toStr().c_str());
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
            log(V4_VVER, "%s : already completed\n", req.toStr().c_str());
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
        log(V4_VVER, "Req. %s : job unknown\n", req.toStr().c_str());
        return true;
    }

    Job& job = get(req.jobId);
    if (job.getState() != ACTIVE) {
        // Job is not active
        log(V4_VVER, "Req. %s : job inactive (%s)\n", job.toStr(), job.jobStateToStr());
        return true;
    }
    if (req.requestedNodeIndex != job.getJobTree().getLeftChildIndex() 
            && req.requestedNodeIndex != job.getJobTree().getRightChildIndex()) {
        // Requested node index is not a valid child index for this job
        log(V4_VVER, "Req. %s : not a valid child index (any more)\n", job.toStr());
        return true;
    }
    if (req.revision < job.getRevision()) {
        // Job was updated in the meantime
        log(V4_VVER, "Req. %s : rev. %i not up to date\n", job.toStr(), req.revision);
        return true;
    }
    if (alreadyAccepted) {
        return false;
    }
    if (req.requestedNodeIndex == job.getJobTree().getLeftChildIndex() && job.getJobTree().hasLeftChild()) {
        // Job already has a left child
        log(V4_VVER, "Req. %s : already has left child\n", job.toStr());
        return true;

    }
    if (req.requestedNodeIndex == job.getJobTree().getRightChildIndex() && job.getJobTree().hasRightChild()) {
        // Job already has a right child
        log(V4_VVER, "Req. %s : already has right child\n", job.toStr());
        return true;
    }
    return false;
}

void JobDatabase::commit(JobRequest& req) {
    if (has(req.jobId)) {
        get(req.jobId).commit(req);
        _has_commitment = true;
        if (_coll_assign) _coll_assign->setStatusDirty();
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

    if (has(req.jobId)) {
        // Know that the job already finished?
        Job& job = get(req.jobId);
        if (job.getState() == PAST) {
            log(V4_VVER, "Reject req. %s : past job\n", 
                            toStr(req.jobId, req.requestedNodeIndex).c_str());
            if (_coll_assign) _coll_assign->setStatusDirty();
            return DISCARD;
        }
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

    // Node is idle and not committed to another job
    if (isIdle()) {
        if (mode != TARGETED_REJOIN) return ADOPT_FROM_IDLE;
        // Oneshot request: Job must be present and suspended
        else if (hasDormantJob(req.jobId)) {
            return ADOPT_FROM_IDLE;
        } else {
            if (_coll_assign) _coll_assign->setStatusDirty();
            return REJECT;
        }
    }

    // Request for a root node:
    // Possibly adopt the job while dismissing the active job
    if (req.requestedNodeIndex == 0) {

        // Adoption only works if this node does not yet compute for that job
        if (!has(req.jobId) || get(req.jobId).getState() != ACTIVE) {

            // Current job must be a non-root leaf node
            Job& job = getActive();
            if (job.getState() == ACTIVE && !job.getJobTree().isRoot() && job.getJobTree().isLeaf()) {
                
                // Inform parent node of the original job  
                log(V4_VVER, "Suspend %s ...\n", job.toStr());
                log(V4_VVER, "... to adopt starving %s\n", 
                                toStr(req.jobId, req.requestedNodeIndex).c_str());

                removedJob = job.getId();
                suspend(removedJob);
                return ADOPT_REPLACE_CURRENT;
            }
        }

        // Adoption did not work out: Defer the request if a certain #hops is reached
        if (req.numHops > 0 && req.numHops % std::max(32, MyMpi::size(_comm)) == 0) {
            log(V3_VERB, "Defer %s\n", req.toStr().c_str());
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
    log(LOG_ADD_SRCRANK | V3_VERB, "RESUME %s", source, 
                toStr(req.jobId, req.requestedNodeIndex).c_str());
    job.resume();
    _balancer->onActivate(job);
}

void JobDatabase::suspend(int jobId) {
    assert(has(jobId) && get(jobId).getState() == ACTIVE);
    Job& job = get(jobId);
    job.suspend();
    setLoad(0, jobId);
    log(V3_VERB, "SUSPEND %s\n", job.toStr());
    _balancer->onSuspend(job);
}

void JobDatabase::terminate(int jobId) {
    Job& job = get(jobId);
    bool wasTerminated = job.getState() == JobState::PAST;
    if (!isIdle() && getActive().getId() == jobId) {
        setLoad(0, jobId);
    }

    if (!wasTerminated) {
        // Gather statistics
        auto numDesires = job.getJobTree().getNumDesires();
        auto numFulfilledDesires = job.getJobTree().getNumFulfiledDesires();
        auto sumDesireLatencies = job.getJobTree().getSumOfDesireLatencies();
        float desireFulfilmentRatio = numDesires == 0 ? 0 : (float)numFulfilledDesires / numDesires;
        float meanFulfilmentLatency = numFulfilledDesires == 0 ? 0 : sumDesireLatencies / numFulfilledDesires;

        auto latencies = job.getJobTree().getDesireLatencies();
        float meanLatency = 0, minLatency = 0, maxLatency = 0, medianLatency = 0;
        if (!latencies.empty()) {
            std::sort(latencies.begin(), latencies.end());
            meanLatency = std::accumulate(latencies.begin(), latencies.end(), 0.0f) / latencies.size();
            minLatency = latencies.front();
            maxLatency = latencies.back();
            medianLatency = latencies[latencies.size()/2];
        }

        log(V3_VERB, "%s desires fulfilled=%.4f latency={num:%i min:%.5f med:%.5f avg:%.5f max:%.5f}\n",
            job.toStr(), desireFulfilmentRatio, latencies.size(), minLatency, medianLatency, meanLatency, maxLatency);
    }

    //_sys_state.addLocal(SYSSTATE_NUMDESIRES, numDesires);
    //_sys_state.addLocal(SYSSTATE_NUMFULFILLEDDESIRES, numFulfilledDesires);
    //_sys_state.addLocal(SYSSTATE_SUMDESIRELATENCIES, sumDesireLatencies);

    job.terminate();
    if (job.hasCommitment()) uncommit(jobId);
    if (!wasTerminated) _balancer->onTerminate(job);
}

void JobDatabase::forgetOldJobs() {

    log(V5_DEBG, "scan for old jobs\n");

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
        // Past jobs
        if (job.getState() == PAST) {
            // If job is past, it must have been so for at least 3 seconds
            if (job.getAgeSinceAbort() < 3) continue;
            // If the node found a result, it must have been already transferred
            if (job.isResultTransferPending()) continue;
            jobsToForget.push_back(id);
        }
        // Suspended job: Forget w.r.t. age, but only if there is a limit on the job cache
        if (job.getState() == SUSPENDED && jobCacheSize > 0) {
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
        log(V4_VVER, "%i resident jobs\n", _jobs.size());
    
    // Perform forgetting of jobs
    for (int jobId : jobsToForget) {
        forget(jobId);
    }
}

void JobDatabase::forget(int jobId) {
    Job& job = get(jobId);
    job.terminate();
    assert(job.getState() == PAST);
    // Check if the job can be destructed
    if (job.isDestructible()) {
        log(V3_VERB, "FORGET #%i\n", jobId);
        free(jobId);
    }
}

void JobDatabase::free(int jobId) {

    if (!has(jobId)) return;
    Job* job = &get(jobId);
    log(V4_VVER, "Delete %s\n", job->toStr());

    // Delete job and its solvers
    _jobs.erase(jobId);

    // Concurrently free associated resources
    {
        auto lock = _janitor_mutex.getLock();
        _jobs_to_free.emplace_back(job);
    }
}

std::vector<std::pair<JobRequest, int>>  JobDatabase::getDeferredRequestsToForward(float time) {
    
    std::vector<std::pair<JobRequest, int>> result;
    for (auto& [deferredTime, senderRank, req] : _deferred_requests) {
        if (time - deferredTime < 1.0f) break;
        result.emplace_back(std::move(req), senderRank);
        log(V3_VERB, "Reactivate deferred %s\n", req.toStr().c_str());
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
        log(V3_VERB, "LOAD 1 (+%s)\n", get(whichJobId).toStr());
        _current_job = &get(whichJobId);
        _time_of_last_adoption = Timer::elapsedSeconds();
    }
    if (load == 0) {
        assert(_current_job != NULL);
        log(V3_VERB, "LOAD 0 (-%s)\n", get(whichJobId).toStr());
        _current_job = NULL;
        float timeBusy = Timer::elapsedSeconds() - _time_of_last_adoption;
        _total_busy_time += timeBusy;
    }
    if (_coll_assign) _coll_assign->setStatusDirty();
}

bool JobDatabase::isIdle() const {
    return _load == 0;
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

JobDatabase::~JobDatabase() {

    log(V3_VERB, "busytime=%.3f\n", _total_busy_time);

    // Setup a watchdog to get feedback on hanging destructors
    Watchdog watchdog(/*checkIntervMillis=*/200, Timer::elapsedSeconds());
    watchdog.setWarningPeriod(500);
    watchdog.setAbortPeriod(10*1000);

    // Collect all jobs 
    std::vector<int> jobIds;
    for (auto idJobPair : _jobs) jobIds.push_back(idJobPair.first);
    
    // Delete each job
    for (int jobId : jobIds) {
        free(jobId);
        watchdog.reset();
    }

    _janitor.stop();
    watchdog.stop();
}