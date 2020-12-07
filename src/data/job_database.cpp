
#include "job_database.hpp"

#include <assert.h>
#include <algorithm>
#include <queue>
#include <utility>

#include "app/sat/forked_sat_job.hpp"
#include "app/sat/threaded_sat_job.hpp"
#include "util/sys/timer.hpp"
#include "util/console.hpp"
#include "balancing/cutoff_priority_balancer.hpp"
#include "balancing/event_driven_balancer.hpp"

JobDatabase::JobDatabase(Parameters& params, MPI_Comm& comm): 
        _params(params), _comm(comm) {
    _threads_per_job = params.getIntParam("t");
    _wcsecs_per_instance = params.getFloatParam("job-wallclock-limit");
    _cpusecs_per_instance = params.getFloatParam("job-cpu-limit");
    _load = 0;
    _last_load_change = Timer::elapsedSeconds();
    _current_job = NULL;
    _load_factor = params.getFloatParam("l");
    assert(0 < _load_factor && _load_factor <= 1.0);
    _balance_period = params.getFloatParam("p");       

    // Initialize balancer
    //balancer = std::unique_ptr<Balancer>(new ThermodynamicBalancer(comm, params));
    if (params.getParam("bm") == "ed") {
        // Event-driven balancing
        _balancer = std::unique_ptr<Balancer>(new EventDrivenBalancer(comm, params));
    } else {
        // Fixed-period balancing
        _balancer = std::unique_ptr<Balancer>(new CutoffPriorityBalancer(comm, params));
    }
}

Job& JobDatabase::createJob(int commSize, int worldRank, int jobId) {

    if (_params.getParam("appmode") == "fork") {
        _jobs[jobId] = new ForkedSatJob(_params, commSize, worldRank, jobId);
    } else {
        _jobs[jobId] = new ThreadedSatJob(_params, commSize, worldRank, jobId);
    }
    return *_jobs[jobId];
}

void JobDatabase::init(int jobId, std::shared_ptr<std::vector<uint8_t>> description, int source) {

    if (!has(jobId) || get(jobId).isPast()) {
        Console::log(Console::WARN, "[WARN] Unknown or past job #%i : discard desc.", jobId);
        return;
    }

    // Erase job commitment
    if (_commitments.count(jobId)) _commitments.erase(jobId);

    // Empty job description
    if (description->size() == sizeof(int)) {
        Console::log(Console::VERB, "Received empty desc. of #%i - uncommit", jobId);
        if (get(jobId).isCommitted()) get(jobId).uncommit();
        return;
    }

    // Initialize job inside a separate thread
    setLoad(1, jobId);
    get(jobId).beginInitialization();
    Console::log(Console::VERB, "Received desc. of #%i - initializing", jobId);
    assert(!_initializer_threads.count(jobId) || Console::fail("%s already has an initializer thread!", get(jobId).toStr()));

    _initializer_threads[jobId] = std::thread([this, description, jobId, source]() {

        // Deserialize job description
        assert(description->size() >= sizeof(int));
        Console::log_recv(Console::VERB, source, 
                "Deserialize job #%i, desc. of size %i", jobId, description->size());
        Job& job = get(jobId);
        job.setDescription(description);
        assert(job.getDescription().getPriority() > 0 && job.getDescription().getPriority() <= 1.0 
            || Console::fail("%s has priority %.2f!", job.toStr(), job.getDescription().getPriority()));

        // Remember arrival and initialize used CPU time (if root node)
        _arrivals[jobId] = Timer::elapsedSeconds();
            
        if (job.isPast()) {
            // Job was already aborted
            job.terminate();
        } else if (!job.isForgetting()) {
            // Initialize job
            job.initialize();
        }

        Console::log(Console::VVVVERB, "%s : init thread done", job.toStr());
    });
}

bool JobDatabase::checkComputationLimits(int jobId) {

    if (!get(jobId).isRoot()) return false;

    if (!_last_limit_check.count(jobId) || !_cpu_time_used.count(jobId)) {
        // Job is new
        _last_limit_check[jobId] = Timer::elapsedSeconds();
        _cpu_time_used[jobId] = 0;
        return false;
    }

    float elapsedTime = Timer::elapsedSeconds() - _last_limit_check[jobId];
    bool terminate = false;
    assert(elapsedTime >= 0);

    // Calculate CPU seconds: (volume during last epoch) * #threads * (effective time of last epoch) 
    float newCpuTime = (_volumes.count(jobId) ? _volumes[jobId] : 1) * _threads_per_job * elapsedTime;
    assert(newCpuTime >= 0);
    _cpu_time_used[jobId] += newCpuTime;
    
    if ((_cpusecs_per_instance > 0 && _cpu_time_used[jobId] > _cpusecs_per_instance)
        || (get(jobId).getDescription().getCpuLimit() > 0 && 
            _cpu_time_used[jobId] > get(jobId).getDescription().getCpuLimit())) {
        // Job exceeded its cpu time limit
        Console::log(Console::INFO, "#%i CPU TIMEOUT: aborting", jobId);
        terminate = true;

    } else {
        // Calculate wall clock time
        float jobAge = get(jobId).getAge();
        if ((_wcsecs_per_instance > 0 && jobAge > _wcsecs_per_instance) 
            || (get(jobId).getDescription().getWallclockLimit() > 0 && 
                jobAge > get(jobId).getDescription().getWallclockLimit())) {
            // Job exceeded its wall clock time limit
            Console::log(Console::INFO, "#%i WALLCLOCK TIMEOUT: aborting", jobId);
            terminate = true;
        }
    }
    
    if (terminate) _last_limit_check.erase(jobId);
    else _last_limit_check[jobId] = Timer::elapsedSeconds();
    
    return terminate;
}


void JobDatabase::forget(int jobId) {
    Job& job = get(jobId);
    Console::log(Console::VVVERB, "Terminate %s to forget", job.toStr());
    if (job.isSuspended()) {
        job.stop();
        job.terminate();
    }
    job.setForgetting();
    // Check if the job can be destructed
    if (job.isDestructible()) {
        free(jobId);
        Console::log(Console::VVVERB, "Forgot #%i", jobId);
    }
}

void JobDatabase::free(int jobId) {

    if (!has(jobId)) return;
    Job& job = get(jobId);
    int index = job.getIndex();
    Console::log(Console::VVERB, "Delete %s", job.toStr());

    // Join and delete initializer thread
    if (_initializer_threads.count(jobId)) {
        Console::log(Console::VVVERB, "Delete init thread of %s", job.toStr());
        if (_initializer_threads[jobId].joinable()) {
            _initializer_threads[jobId].join();
        }
    }

    // Remove map entries
    _commitments.erase(jobId);
    _arrivals.erase(jobId);
    _cpu_time_used.erase(jobId);  
    _last_limit_check.erase(jobId);      
    _volumes.erase(jobId);  
    _initializer_threads.erase(jobId);

    // Remove job meta data from balancer
    _balancer->forget(jobId);

    // Delete job and its solvers
    _jobs.erase(jobId);
    delete &job;

    Console::log(Console::VERB, "Deleted %s", toStr(jobId, index).c_str());
    Console::mergeJobLogs(jobId);
}

bool JobDatabase::isRequestObsolete(const JobRequest& req) {

    // Requests for a job root never become obsolete
    if (req.requestedNodeIndex == 0) return false;

    // Does this node KNOW that the request is already completed?
    if (!isIdle() && req.jobId == getActive().getId()) {
        Job& job = get(req.jobId);
        if (req.requestedNodeIndex == job.getIndex()
        || (job.hasLeftChild() && req.requestedNodeIndex == job.getLeftChildIndex())
        || (job.hasRightChild() && req.requestedNodeIndex == job.getRightChildIndex())) {
            // Request completed!
            Console::log(Console::VERB, "Req. %s : already completed", job.toStr());
            return true;
        }
        if (job.isPast()) {
            // Job has already terminated!
            Console::log(Console::VERB, "Req. %s : past job", job.toStr());
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
    if (!job.isActive()) {
        // Job is not active
        Console::log(Console::VERB, "Req. %s : job inactive (%s)", job.toStr(), job.jobStateToStr());
        return true;
    
    } else if (alreadyAccepted) {
        return false;

    } else if (req.requestedNodeIndex == job.getLeftChildIndex() && job.hasLeftChild()) {
        // Job already has a left child
        Console::log(Console::VERB, "Req. %s : already has left child", job.toStr());
        return true;

    } else if (req.requestedNodeIndex == job.getRightChildIndex() && job.hasRightChild()) {
        // Job already has a right child
        Console::log(Console::VERB, "Req. %s : already has right child", job.toStr());
        return true;

    } else if (req.requestedNodeIndex != job.getLeftChildIndex() 
            && req.requestedNodeIndex != job.getRightChildIndex()) {
        // Requested node index is not a valid child index for this job
        Console::log(Console::VERB, "Req. %s : not a valid child index (any more)", job.toStr());
        return true;
    }

    return false;
}

void JobDatabase::commit(JobRequest& req) {
    _commitments[req.jobId] = req;
    if (has(req.jobId)) get(req.jobId).commit(req);
}

bool JobDatabase::hasCommitments() const {
    return !_commitments.empty();
}

bool JobDatabase::hasCommitment(int jobId) const {
    return _commitments.count(jobId);
}

JobRequest& JobDatabase::getCommitment(int jobId) {
    return _commitments.at(jobId);
}

void JobDatabase::uncommit(int jobId) {
    _commitments.erase(jobId);
    if (has(jobId) && get(jobId).isCommitted()) get(jobId).uncommit();
}

bool JobDatabase::tryAdopt(const JobRequest& req, bool oneshot, int& removedJob) {

    // Decide whether job should be adopted or bounced to another node
    bool adopts = false;
    removedJob = -1;
    
    if (has(req.jobId) && (get(req.jobId).isPast() || get(req.jobId).isForgetting())) {
        // Can mean that the job finished in the meantime or that
        // it is in the process of being cleaned up.
        Console::log(Console::VERB, "Reject req. %s : PAST/FORGETTING", 
                        toStr(req.jobId, req.requestedNodeIndex).c_str());

    } else if (isIdle() && !hasCommitments()) {
        // Node is idle and not committed to another job: OK

        if (oneshot) {
            // Oneshot request: Job must be present and suspended
            adopts = has(req.jobId) && get(req.jobId).isSuspended();
        } else adopts = true;

    } else if (req.requestedNodeIndex == 0 
            && req.numHops > 32 //std::max(50, MyMpi::size(comm)/2)
            && !hasCommitments()) {
        // Request for a root node exceeded max #hops: 
        // Possibly adopt the job while dismissing the active job

        // Consider adoption only if that job is unknown or inactive 
        if (!has(req.jobId) || !get(req.jobId).isActive()) {

            // Look for an active job that can be suspended
            if (!isIdle()) {
                Job& job = getActive();
                // Job must be active and a non-root leaf node
                if (job.isActive() && !job.isRoot() && job.isLeaf()) {
                    
                    // Inform parent node of the original job  
                    Console::log(Console::VERB, "Suspend %s ...", job.toStr());
                    Console::log(Console::VERB, "... to adopt starving %s", 
                                    toStr(req.jobId, req.requestedNodeIndex).c_str());

                    removedJob = job.getId();
                    suspend(removedJob);
                    adopts = true;
                }
            }
        }
    }

    return adopts;
}

void JobDatabase::reactivate(const JobRequest& req, int source) {
    // Already has job description: Directly resume job (if not terminated yet)
    assert(has(req.jobId));
    Job& job = get(req.jobId);
    if (!job.hasJobDescription() && !job.isInitializing()) {
        Console::log(Console::WARN, "[WARN] %s has no desc. although full transfer was not requested", job.toStr());
    } else if (!job.isPast()) {
        Console::log_recv(Console::INFO, source, "Reactivate %s (state: %s)", 
                    toStr(req.jobId, req.requestedNodeIndex).c_str(), job.jobStateToStr());
        setLoad(1, req.jobId);
        job.reactivate(req.requestedNodeIndex, req.rootRank, req.requestingNodeRank);
    }
    // Erase job commitment
    uncommit(req.jobId);
}

void JobDatabase::suspend(int jobId) {
    assert(has(jobId) && !isIdle() && getActive().getId() == jobId);
    get(jobId).suspend();
    setLoad(0, jobId);
}

void JobDatabase::stop(int jobId, bool terminate) {
    Job& job = get(jobId);
    if (job.isInitializing() || job.isActive() || job.isSuspended()) {
        Console::log(Console::INFO, "%s : interrupt (state: %s)", job.toStr(), job.jobStateToStr());
        job.stop(); // Solvers are interrupted, not suspended!
        Console::log(Console::VERB, "%s : interrupted", job.toStr());
        if (terminate) {
            if (!isIdle() && getActive().getId() == jobId) setLoad(0, jobId);
            job.terminate();
            Console::log(Console::INFO, "%s : terminated", job.toStr());
            overrideBalancerVolume(jobId, 0);
        } 
    }
    // Mark committed job as "PAST"
    if (job.isCommitted() && terminate) {
        uncommit(jobId);
        job.terminate();
    }
}

void JobDatabase::forgetOldJobs() {

    std::vector<int> jobsToForget;
    int jobCacheSize = _params.getIntParam("jc");

    // Scan jobs for being forgettable
    std::priority_queue<std::pair<int, float>, std::vector<std::pair<int, float>>, SuspendedJobComparator> suspendedQueue;
    for (auto idJobPair : _jobs) {
        int id = idJobPair.first;
        Job& job = *idJobPair.second;
        // Job must be finished initializing
        if (job.isInitializing()) {
            // Check end of initialization for inactive jobs
            if (!job.isActive() && job.isDoneInitializing()) job.endInitialization();
            continue;
        }
        // Jobs that were never active
        if (job.isInState({NONE}) && job.getAge() >= 60) {
            jobsToForget.push_back(id);
            continue;
        }
        // Suspended jobs: Forget w.r.t. age, but only if there is a limit on the job cache
        if (jobCacheSize > 0 && job.isSuspended()) {
            // Job must not be rooted here
            if (job.isRoot()) continue;
            // Insert job into PQ according to its age 
            float age = job.getAgeSinceActivation();
            suspendedQueue.emplace(id, age);
        }
        // Past jobs
        if (job.isPast() || job.isForgetting()) {
            // If job is past, it must have been so for at least 60 seconds
            if (job.getAgeSinceAbort() < 60) continue;
            // If the node found a result, it must have been already transferred
            if (job.isResultTransferPending()) continue;
            jobsToForget.push_back(id);
        }
    }

    // Mark jobs as forgettable as long as job cache is exceeded
    while ((int)suspendedQueue.size() > jobCacheSize) {
        jobsToForget.push_back(suspendedQueue.top().first);
        suspendedQueue.pop();
    }

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
    _last_load_change = now;
    assert(has(whichJobId));
    if (load == 1) {
        assert(_current_job == NULL);
        Console::log(Console::VERB, "LOAD 1 (+%s)", get(whichJobId).toStr());
        _current_job = &get(whichJobId);
    }
    if (load == 0) {
        assert(_current_job != NULL);
        Console::log(Console::VERB, "LOAD 0 (-%s)", get(whichJobId).toStr());
        _current_job = NULL;
    }
}

bool JobDatabase::isIdle() const {
    return _load == 0;
}

bool JobDatabase::isTimeForRebalancing() {
    return !_balancer->isBalancing() && _epoch_counter.getSecondsSinceLastSync() >= _balance_period;
}

bool JobDatabase::beginBalancing() {

    // Initiate balancing procedure
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

bool JobDatabase::continueBalancing(MessageHandlePtr& handle) {
    bool done = _balancer->continueBalancing(handle);
    if (done) finishBalancing();
    return done;
}

void JobDatabase::finishBalancing() {

    // All collective operations are done; reset synchronized timer
    _epoch_counter.resetLastSync();

    // Retrieve balancing results
    Console::log(Console::VVVERB, "Finishing balancing ...");
    _volumes = _balancer->getBalancingResult();
    Console::log(MyMpi::rank(MPI_COMM_WORLD) == 0 ? Console::VERB : Console::VVVERB, "Balancing completed.");

    // Add last slice of idle/busy time 
    //stats.add((load == 1 ? "busyTime" : "idleTime"), Timer::elapsedSeconds() - lastLoadChange);
    _last_load_change = Timer::elapsedSeconds();

    // Advance to next epoch
    _epoch_counter.increment();
    Console::log(Console::VVVERB, "Advancing to epoch %i", _epoch_counter.getEpoch());
}

const std::map<int, int>& JobDatabase::getBalancingResult() {
    return _balancer->getBalancingResult();
}

bool JobDatabase::hasVolume(int jobId) {
    return _balancer->hasVolume(jobId);
}

int JobDatabase::getVolume(int jobId) {
    return _balancer->getVolume(jobId);
}

void JobDatabase::overrideBalancerVolume(int jobId, int volume) {
    _balancer->updateVolume(jobId, volume);
}

JobDatabase::~JobDatabase() {
    // Delete each job (iterating over "jobs" invalid as entries are deleted)
    std::vector<int> jobIds;
    for (auto idJobPair : _jobs) jobIds.push_back(idJobPair.first);
    for (int jobId : jobIds) free(jobId);
}