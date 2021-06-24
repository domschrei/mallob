
#include "job_database.hpp"

#include <assert.h>
#include <algorithm>
#include <queue>
#include <utility>

#include "app/sat/forked_sat_job.hpp"
#include "app/sat/threaded_sat_job.hpp"
#include "util/sys/timer.hpp"
#include "util/logger.hpp"
#include "balancing/event_driven_balancer.hpp"
#include "util/sys/watchdog.hpp"

JobDatabase::JobDatabase(Parameters& params, MPI_Comm& comm): 
        _params(params), _comm(comm) {
    _wcsecs_per_instance = params.jobWallclockLimit();
    _cpusecs_per_instance = params.jobCpuLimit();
    _load = 0;
    _last_balancing_initiation = 0;
    _current_job = NULL;
    _load_factor = params.loadFactor();
    assert(0 < _load_factor && _load_factor <= 1.0);
    _balance_period = params.balancingPeriod();       

    // Initialize balancer
    _balancer = std::unique_ptr<Balancer>(new EventDrivenBalancer(comm, params));

    _janitor = std::thread([this]() {
        log(V3_VERB, "Job-DB Janitor tid=%lu\n", Proc::getTid());
        while (!_exiting || _num_stored_jobs > 0) {
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

Job& JobDatabase::createJob(int commSize, int worldRank, int jobId) {

    if (_params.applicationSpawnMode() == "fork") {
        _jobs[jobId] = new ForkedSatJob(_params, commSize, worldRank, jobId);
    } else {
        _jobs[jobId] = new ThreadedSatJob(_params, commSize, worldRank, jobId);
    }
    _num_stored_jobs++;
    return *_jobs[jobId];
}

bool JobDatabase::init(int jobId, const std::shared_ptr<std::vector<uint8_t>>& description, int source) {

    if (!has(jobId) || get(jobId).getState() == PAST) {
        log(V1_WARN, "[WARN] Unknown or past job #%i : discard desc. of size %i\n", jobId, description->size());
        return false;
    }
    auto& job = get(jobId);

    // Erase job commitment
    uncommit(jobId);

    // Empty job description
    if (description->size() == sizeof(int)) {
        log(V4_VVER, "Received empty desc. of #%i - uncommit and ignore\n", jobId);
        return false;
    }

    // Initialize job (in a separate thread)
    setLoad(1, jobId);
    log(LOG_ADD_SRCRANK | V3_VERB, "START %s", source, job.toStr());
    get(jobId).start(description);
    return true;
}

bool JobDatabase::restart(int jobId, const std::shared_ptr<std::vector<uint8_t>>& description, int source) {

    if (!has(jobId) || get(jobId).getState() == PAST) {
        log(V1_WARN, "[WARN] Unknown or past job #%i : discard desc. of size %i\n", jobId, description->size());
        return false;
    }
    auto& job = get(jobId);

    // Erase job commitment
    uncommit(jobId);

    // Empty job description
    if (description->size() == sizeof(int)) {
        log(V4_VVER, "Received empty desc. of #%i - uncommit and ignore\n", jobId);
        return false;
    }

    // Restart job (in a separate thread)
    setLoad(1, jobId);
    log(LOG_ADD_SRCRANK | V3_VERB, "RESTART %s", source, job.toStr());
    get(jobId).restart(description);
    return true;
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

    if (has(req.jobId)) {
        Job& job = get(req.jobId);
        if (job.getState() == PAST) {
            // Job has already terminated!
            log(V4_VVER, "%s : past job\n", req.toStr().c_str());
            return true;
        }
        if (job.getState() == ACTIVE) {
            // Does this node KNOW that the request is already completed?
            if (req.requestedNodeIndex == job.getIndex()
            || (job.getJobTree().hasLeftChild() && req.requestedNodeIndex == job.getJobTree().getLeftChildIndex())
            || (job.getJobTree().hasRightChild() && req.requestedNodeIndex == job.getJobTree().getRightChildIndex())) {
                // Request already completed!
                log(V4_VVER, "%s : already completed\n", req.toStr().c_str());
                return true;
            }
            // Am I the transitive parent of the request and do I know
            // that the job's volume does not allow for this node any more? 
            if (job.getVolume() > 0 && job.getVolume() <= req.requestedNodeIndex 
                    && job.getJobTree().isTransitiveParentOf(req.requestedNodeIndex)) {
                log(V4_VVER, "%s : new volume too small\n", req.toStr().c_str());
                return true;
            }
        }
    }
}

bool JobDatabase::isAdoptionOfferObsolete(const JobRequest& req, bool alreadyAccepted) {

    // Requests for a job root never become obsolete
    if (req.requestedNodeIndex == 0) return false;

    // Job not known anymore: obsolete
    if (!has(req.jobId)) return true;

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
    if (req.currentRevision < job.getRevision()) {
        // Job was updated in the meantime
        log(V4_VVER, "Req. %s : rev. %i not up to date\n", job.toStr(), req.currentRevision);
        return true;
    }
    // Does the job's volume not allow for this node any more? 
    if (job.getVolume() > 0 && job.getVolume() <= req.requestedNodeIndex) {
        log(V4_VVER, "Req. %s : new volume too small\n", req.toStr().c_str());
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
    }
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
    }
}

JobDatabase::AdoptionResult JobDatabase::tryAdopt(const JobRequest& req, bool oneshot, int sender, int& removedJob) {

    // Decide whether job should be adopted or bounced to another node
    removedJob = -1;
    
    // Already have another commitment?
    if (_has_commitment) return REJECT;

    if (has(req.jobId)) {
        // Know that the job already finished?
        Job& job = get(req.jobId);
        if (job.getState() == PAST) {
            log(V4_VVER, "Reject req. %s : already finished\n", 
                            toStr(req.jobId, req.requestedNodeIndex).c_str());
            return DISCARD;
        }
    }

    bool isThisDormantRoot = has(req.jobId) && get(req.jobId).getJobTree().isRoot();

    if (!isThisDormantRoot && hasDormantRoot() && req.requestedNodeIndex == 0) {
        // Cannot adopt a root node while there is still another dormant root here
        return REJECT;
    }


    // Node is idle and not committed to another job
    if (isIdle()) {
        if (!oneshot) return ADOPT_FROM_IDLE;
        // Oneshot request: Job must be present and suspended
        else return (has(req.jobId) && 
            (get(req.jobId).getState() == SUSPENDED || get(req.jobId).getState() == STANDBY) 
                ? ADOPT_FROM_IDLE : REJECT);
    }

    // Request for a root node which either exceeded a large number of hops
    // or which already resides on this node as a dormant root:
    // Possibly adopt the job while dismissing the active job
    if (req.requestedNodeIndex == 0 && (req.numHops >= 32 || isThisDormantRoot)) {

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

    return REJECT;
}

void JobDatabase::reactivate(const JobRequest& req, int source) {
    // Already has job description: Directly resume job (if not terminated yet)
    assert(has(req.jobId));
    Job& job = get(req.jobId);
    job.updateJobTree(req.requestedNodeIndex, req.rootRank, req.requestingNodeRank);
    setLoad(1, req.jobId);
    if (job.getState() == SUSPENDED) {
        log(LOG_ADD_SRCRANK | V3_VERB, "RESUME %s", source, 
                    toStr(req.jobId, req.requestedNodeIndex).c_str());
        job.resume();
    } else if (job.getState() == INACTIVE) {
        log(LOG_ADD_SRCRANK | V3_VERB, "RESTART %s", source, 
                    toStr(req.jobId, req.requestedNodeIndex).c_str());
        abort(); // TODO not implemented yet
    }
}

void JobDatabase::suspend(int jobId) {
    assert(has(jobId) && get(jobId).getState() == ACTIVE);
    get(jobId).suspend();
    setLoad(0, jobId);
    log(V3_VERB, "SUSPEND %s\n", get(jobId).toStr());
}

void JobDatabase::stop(int jobId, bool terminate) {
    Job& job = get(jobId);
    if (!isIdle() && getActive().getId() == jobId) setLoad(0, jobId);
    if (job.hasCommitment()) uncommit(jobId);
    if (job.getState() == SUSPENDED) job.resume();
    if (terminate) {
        if (job.getState() == ACTIVE) job.stop();
        if (job.getState() == INACTIVE || job.getState() == STANDBY) {
            job.terminate();
            log(V3_VERB, "TERMINATE %s\n", job.toStr());
        } 
    } else {
        log(V3_VERB, "STOP %s (state: %s)\n", job.toStr(), job.jobStateToStr());
        if (job.getState() == ACTIVE) job.interrupt();
    }
}

void JobDatabase::forgetOldJobs() {

    std::vector<int> jobsToForget;
    int jobCacheSize = _params.jobCacheSize();
    size_t numJobsWithDescription = 0;

    // Scan jobs for being forgettable
    std::priority_queue<std::pair<int, float>, std::vector<std::pair<int, float>>, SuspendedJobComparator> suspendedQueue;
    for (auto [id, jobPtr] : _jobs) {
        Job& job = *jobPtr;
        if (job.hasReceivedDescription()) numJobsWithDescription++;
        if (job.hasCommitment()) continue;
        // Old inactive job
        if (job.getState() == INACTIVE && job.getAge() >= 10) {
            jobsToForget.push_back(id);
            continue;
        }
        // Past jobs
        if (job.getState() == PAST) {
            // If job is past, it must have been so for at least 10 seconds
            if (job.getAgeSinceAbort() < 10) continue;
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
        log(V3_VERB, "%i resident jobs\n", _jobs.size());
    
    // Perform forgetting of jobs
    for (int jobId : jobsToForget) {
        forget(jobId);
    }
}

void JobDatabase::forget(int jobId) {
    Job& job = get(jobId);
    if (job.getState() == SUSPENDED) job.resume();
    if (job.getState() == ACTIVE) job.stop();
    if (job.getState() == INACTIVE) job.terminate();
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

    // Remove job meta data from balancer
    _balancer->forget(jobId);

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
    }
    if (load == 0) {
        assert(_current_job != NULL);
        log(V3_VERB, "LOAD 0 (-%s)\n", get(whichJobId).toStr());
        _current_job = NULL;
    }
}

bool JobDatabase::isIdle() const {
    return _load == 0;
}

bool JobDatabase::isTimeForRebalancing() {
    return !_balancer->isBalancing() 
        && Timer::elapsedSeconds() - _last_balancing_initiation >= _balance_period;
}

bool JobDatabase::beginBalancing() {

    // Initiate balancing procedure
    _last_balancing_initiation = Timer::elapsedSeconds();
    bool done = _balancer->beginBalancing(_jobs);
    // If nothing to do, finish up balancing
    if (done) finishBalancing();
    return done;
}

bool JobDatabase::continueBalancing() {
    if (_balancer->isBalancing()) {
        // Advance balancing if possible (e.g. an iallreduce finished)
        if (_balancer->canContinueBalancing()) {
            bool done = _balancer->continueBalancing();
            if (done) finishBalancing();
            return done;
        }
    }
    return false;
}

bool JobDatabase::continueBalancing(MessageHandle& handle) {
    bool done = _balancer->continueBalancing(handle);
    if (done) finishBalancing();
    return done;
}

void JobDatabase::finishBalancing() {
    log(MyMpi::rank(MPI_COMM_WORLD) == 0 ? V3_VERB : V5_DEBG, "Balancing completed.\n");
}

robin_hood::unordered_map<int, int> JobDatabase::getBalancingResult() {
    return _balancer->getBalancingResult();
}

bool JobDatabase::hasDormantRoot() const {
    for (auto& [_, job] : _jobs) {
        if (job->getJobTree().isRoot() && job->getState() == STANDBY) 
            return true;
    }
    return false;
}

JobDatabase::~JobDatabase() {

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

    // Join cleanup thread for old jobs
    _exiting = true;
    if (_janitor.joinable()) _janitor.join();

    watchdog.stop();
}