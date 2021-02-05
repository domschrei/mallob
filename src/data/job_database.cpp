
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
    _threads_per_job = params.getIntParam("t");
    _wcsecs_per_instance = params.getFloatParam("job-wallclock-limit");
    _cpusecs_per_instance = params.getFloatParam("job-cpu-limit");
    _load = 0;
    _last_balancing_initiation = 0;
    _current_job = NULL;
    _load_factor = params.getFloatParam("l");
    assert(0 < _load_factor && _load_factor <= 1.0);
    _balance_period = params.getFloatParam("p");       

    // Initialize balancer
    //balancer = std::unique_ptr<Balancer>(new ThermodynamicBalancer(comm, params));
    _balancer = std::unique_ptr<Balancer>(new EventDrivenBalancer(comm, params));
}

Job& JobDatabase::createJob(int commSize, int worldRank, int jobId) {

    if (_params.getParam("appmode") == "fork") {
        _jobs[jobId] = new ForkedSatJob(_params, commSize, worldRank, jobId);
    } else {
        _jobs[jobId] = new ThreadedSatJob(_params, commSize, worldRank, jobId);
    }
    return *_jobs[jobId];
}

void JobDatabase::init(int jobId, std::vector<uint8_t>&& description, int source) {

    if (!has(jobId) || get(jobId).getState() == PAST) {
        log(V1_WARN, "[WARN] Unknown or past job #%i : discard desc.\n", jobId);
        return;
    }
    auto& job = get(jobId);

    // Erase job commitment
    uncommit(jobId);

    // Empty job description
    if (description.size() == sizeof(int)) {
        log(V4_VVER, "Received empty desc. of #%i - uncommit and ignore\n", jobId);
        return;
    }

    // Initialize job (in a separate thread)
    setLoad(1, jobId);
    log(LOG_ADD_SRCRANK | V3_VERB, "START %s", source, job.toStr());
    get(jobId).start(std::move(description));
}

bool JobDatabase::checkComputationLimits(int jobId) {

    auto& job = get(jobId);
    if (!job.getJobTree().isRoot()) return false;
    return job.checkResourceLimit(_wcsecs_per_instance, _cpusecs_per_instance);
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
    int index = job->getIndex();
    log(V4_VVER, "Delete %s\n", job->toStr());

    // Remove job meta data from balancer
    _balancer->forget(jobId);

    // Delete job and its solvers
    _jobs.erase(jobId);
    delete job;

    log(V4_VVER, "Deleted %s\n", toStr(jobId, index).c_str());
    Logger::getMainInstance().mergeJobLogs(jobId);
}

bool JobDatabase::isRequestObsolete(const JobRequest& req) {

    // Requests for a job root never become obsolete
    if (req.requestedNodeIndex == 0) return false;

    // Does this node KNOW that the request is already completed?
    if (!isIdle() && req.jobId == getActive().getId()) {
        Job& job = get(req.jobId);
        if (req.requestedNodeIndex == job.getIndex()
        || (job.getJobTree().hasLeftChild() && req.requestedNodeIndex == job.getJobTree().getLeftChildIndex())
        || (job.getJobTree().hasRightChild() && req.requestedNodeIndex == job.getJobTree().getRightChildIndex())) {
            // Request completed!
            log(V4_VVER, "Req. %s : already completed\n", job.toStr());
            return true;
        }
        if (job.getState() == PAST) {
            // Job has already terminated!
            log(V4_VVER, "Req. %s : past job\n", job.toStr());
            return true;
        }
    }
    
    float maxAge = _params.getFloatParam("rto"); // request time out
    if (maxAge > 0) {
        //float timelim = 0.25 + 2 * params.getFloatParam("p");
        return Timer::elapsedSeconds() - req.timeOfBirth >= maxAge; 
    } else {
        return false; // not obsolete
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
    
    } else if (req.requestedNodeIndex != job.getJobTree().getLeftChildIndex() 
            && req.requestedNodeIndex != job.getJobTree().getRightChildIndex()) {
        // Requested node index is not a valid child index for this job
        log(V4_VVER, "Req. %s : not a valid child index (any more)\n", job.toStr());
        return true;

    } else if (alreadyAccepted) {
        return false;

    } else if (req.requestedNodeIndex == job.getJobTree().getLeftChildIndex() && job.getJobTree().hasLeftChild()) {
        // Job already has a left child
        log(V4_VVER, "Req. %s : already has left child\n", job.toStr());
        return true;

    } else if (req.requestedNodeIndex == job.getJobTree().getRightChildIndex() && job.getJobTree().hasRightChild()) {
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

bool JobDatabase::tryAdopt(const JobRequest& req, bool oneshot, int& removedJob) {

    // Decide whether job should be adopted or bounced to another node
    removedJob = -1;
    
    // Already have another commitment?
    if (_has_commitment) return false;

    // Know that the job already finished?
    if (has(req.jobId) && get(req.jobId).getState() == PAST) {
        log(V4_VVER, "Reject req. %s : already finished\n", 
                        toStr(req.jobId, req.requestedNodeIndex).c_str());
        return false;
    }

    // Node is idle and not committed to another job
    if (isIdle()) {
        if (!oneshot) return true;
        // Oneshot request: Job must be present and suspended
        else return (has(req.jobId) && get(req.jobId).getState() == SUSPENDED);
    }

    // Request for a root node exceeded max #hops: 
    // Possibly adopt the job while dismissing the active job
    if (req.requestedNodeIndex == 0 && req.numHops > 32) {

        // Does not work if this node already works on that job
        if (has(req.jobId) && get(req.jobId).getState() == ACTIVE)
            return false;

        // Current job must be a non-root leaf node
        Job& job = getActive();
        if (job.getState() == ACTIVE && !job.getJobTree().isRoot() && job.getJobTree().isLeaf()) {
            
            // Inform parent node of the original job  
            log(V4_VVER, "Suspend %s ...\n", job.toStr());
            log(V4_VVER, "... to adopt starving %s\n", 
                            toStr(req.jobId, req.requestedNodeIndex).c_str());

            removedJob = job.getId();
            suspend(removedJob);
            return true;
        }
    }

    return false;
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
        job.start(std::vector<uint8_t>());
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
    if (job.getState() == SUSPENDED) job.resume();
    if (job.getState() == ACTIVE) job.stop();
    if (job.getState() == INACTIVE && terminate) {
        if (!isIdle() && getActive().getId() == jobId) setLoad(0, jobId);
        job.terminate();
        log(V3_VERB, "TERMINATE %s\n", job.toStr());
        if (job.hasCommitment()) uncommit(jobId);
    } else {
        log(V3_VERB, "STOP %s\n", job.toStr());
    }
}

void JobDatabase::forgetOldJobs() {

    std::vector<int> jobsToForget;
    int jobCacheSize = _params.getIntParam("jc");
    size_t numJobsWithDescription = 0;

    // Scan jobs for being forgettable
    std::priority_queue<std::pair<int, float>, std::vector<std::pair<int, float>>, SuspendedJobComparator> suspendedQueue;
    for (auto [id, jobPtr] : _jobs) {
        Job& job = *jobPtr;
        if (job.hasReceivedDescription()) numJobsWithDescription++;
        if (job.hasCommitment()) continue;
        // Old inactive job
        if (job.getState() == INACTIVE && job.getAge() >= 60) {
            jobsToForget.push_back(id);
            continue;
        }
        // Past jobs
        if (job.getState() == PAST) {
            // If job is past, it must have been so for at least 60 seconds
            if (job.getAgeSinceAbort() < 60) continue;
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
        log(V3_VERB, "%i resident jobs, %i with desc.\n", _jobs.size(), numJobsWithDescription);
    
    // Perform forgetting of jobs
    for (int jobId : jobsToForget) {
        forget(jobId);
    }
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

    // Measure time for which worker was {idle,busy}
    float now = Timer::elapsedSeconds();
    //stats.add((load == 0 ? "busyTime" : "idleTime"), now - lastLoadChange);
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

JobDatabase::~JobDatabase() {

    // Setup a watchdog to get feedback on hanging destructors
    Watchdog watchdog(/*checkIntervMillis=*/10*1000, Timer::elapsedSeconds());

    // Collect all jobs 
    std::vector<int> jobIds;
    for (auto idJobPair : _jobs) jobIds.push_back(idJobPair.first);
    
    // Delete each job
    for (int jobId : jobIds) {
        free(jobId);
        watchdog.reset();
    }

    watchdog.stop();
}