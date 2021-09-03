
#include <iostream>
#include <fstream>
#include <string>
#include <chrono>
#include <thread>
#include <unistd.h>
#include <list>

#include "client.hpp"
#include "util/sys/timer.hpp"
#include "util/logger.hpp"
#include "util/permutation.hpp"
#include "util/sys/proc.hpp"
#include "data/job_transfer.hpp"
#include "data/job_result.hpp"
#include "util/random.hpp"
#include "app/sat/sat_constants.h"
#include "util/sys/terminator.hpp"
#include "data/job_reader.hpp"
#include "util/sys/thread_pool.hpp"

// Executed by a separate worker thread
void Client::readIncomingJobs(Logger log) {

    log.log(V3_VERB, "Starting\n");

    while (true) {
        // Wait for a nonempty incoming job queue
        _incoming_job_cond_var.wait(_incoming_job_lock, [&]() {
            return !_instance_reader.continueRunning() 
                || (_num_incoming_jobs > 0 && _num_loaded_jobs < 32);
        });
        if (!_instance_reader.continueRunning()) break;

        // Obtain lock, measure time
        auto lock = _incoming_job_lock.getLock();
        float time = Timer::elapsedSeconds();

        // Find a single job eligible for parsing
        JobMetadata foundJob;
        for (auto& data : _incoming_job_queue) {
            
            // Jobs are sorted by arrival:
            // If this job has not arrived yet, then none have arrived yet
            if (time < data.description->getArrival()) break;

            // Check job's dependencies
            bool dependenciesSatisfied = true;
            {
                auto lock = _done_job_lock.getLock();
                for (int jobId : data.dependencies) {
                    if (!_done_jobs.count(jobId)) {
                        dependenciesSatisfied = false;
                        break;
                    }
                }
                if (data.description->isIncremental() && data.description->getRevision() > 0) {
                    // Check if the precursor of this incremental job is already done
                    if (!_done_jobs.count(data.description->getId()))
                        dependenciesSatisfied = false; // no job with this ID is done yet
                    else if (data.description->getRevision() != _done_jobs[data.description->getId()].revision+1)
                        dependenciesSatisfied = false; // job with correct revision not done yet
                    else {
                        data.description->setChecksum(_done_jobs[data.description->getId()].lastChecksum);
                    }
                }
            }
            if (!dependenciesSatisfied) continue;

            // Job can be read: Enqueue reader task into thread pool
            foundJob = data;
            ProcessWideThreadPool::get().addTask([this, &log, foundJob]() {
                // Read job
                int id = foundJob.description->getId();
                float time = Timer::elapsedSeconds();
                log.log(V3_VERB, "[T] Reading job #%i rev. %i (%s) ...\n", id, foundJob.description->getRevision(), foundJob.file.c_str());
                bool success = JobReader::read(foundJob.file, *foundJob.description);
                if (!success) {
                    log.log(V1_WARN, "[T] [WARN] File %s could not be opened - skipping #%i\n", foundJob.file.c_str(), id);
                } else {
                    time = Timer::elapsedSeconds() - time;
                    log.log(V3_VERB, "[T] Initialized job #%i (%s) in %.3fs: %ld lits w/ separators, %ld assumptions\n", 
                            id, foundJob.file.c_str(), time, foundJob.description->getNumFormulaLiterals(), 
                            foundJob.description->getNumAssumptionLiterals());
                    foundJob.description->getStatistics().parseTime = time;
                    
                    // Enqueue in ready jobs
                    auto lock = _ready_job_lock.getLock();
                    _ready_job_queue.emplace_back(foundJob.description);
                    _num_ready_jobs++;
                    _num_loaded_jobs++;
                    _sys_state.addLocal(SYSSTATE_PARSED_JOBS, 1);
                }
            });
            break;
        }

        if (foundJob.description) {
            // If a job was found, delete it from incoming queue
            _incoming_job_queue.erase(foundJob);
            _num_incoming_jobs--;
        } else {
            // No eligible jobs right now -- sleep for a while
            lock.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    log.log(V3_VERB, "Stopping\n");
    log.flush();
}

void Client::handleNewJob(JobMetadata&& data) {

    if (data.done) {
        // Incremental job notified to be finished
        int jobId = data.description->getId();
        auto lock = _done_job_lock.getLock();
        _recently_done_jobs.insert(jobId);
        return;
    }

    // Introduce new job into "incoming" queue
    data.description->setClientRank(_world_rank);
    {
        auto lock = _incoming_job_lock.getLock();
        _incoming_job_queue.insert(std::move(data));
    }
    _num_incoming_jobs++;
    _incoming_job_cond_var.notify();
    _sys_state.addLocal(SYSSTATE_ENTERED_JOBS, 1);
}

void Client::init() {

    _file_adapter = std::unique_ptr<JobFileAdapter>(
        new JobFileAdapter(getInternalRank(), _params, 
            Logger::getMainInstance().copy("API", "#0."),
            getApiPath(), [&](JobMetadata&& data) {handleNewJob(std::move(data));}
        )
    );
    _instance_reader.run([this]() {
        readIncomingJobs(
            Logger::getMainInstance().copy("<Reader>", "#-1.")
        );
    });
    log(V3_VERB, "Client main thread started\n");

    auto& q = MyMpi::getMessageQueue();
    q.registerCallback(MSG_NOTIFY_JOB_DONE, [&](MessageHandle& h) {handleJobDone(h);});
    q.registerCallback(MSG_SEND_JOB_RESULT, [&](MessageHandle& h) {handleSendJobResult(h);});
    q.registerCallback(MSG_NOTIFY_CLIENT_JOB_ABORTING, [&](MessageHandle& h) {handleAbort(h);});
    q.registerCallback(MSG_OFFER_ADOPTION_OF_ROOT, [&](MessageHandle& h) {handleOfferAdoption(h);});
    q.registerSentCallback([&](int id) {handleJobDescriptionSent(id);});
}

int Client::getInternalRank() {
    return MyMpi::rank(_comm) + _params.firstApiIndex();
}

std::string Client::getApiPath() {
    return ".api/jobs." + std::to_string(getInternalRank()) + "/";
}

void Client::advance() {

    float time = Timer::elapsedSeconds();
    
    // Send notification messages for recently done jobs
    if (_periodic_check_done_jobs.ready()) {
        robin_hood::unordered_flat_set<int, robin_hood::hash<int>> doneJobs;
        {
            auto lock = _done_job_lock.getLock();
            doneJobs = std::move(_recently_done_jobs);
            _recently_done_jobs.clear();
        }
        for (int jobId : doneJobs) {
            log(LOG_ADD_DESTRANK | V3_VERB, "Notify #%i:0 that job is done", _root_nodes[jobId], jobId);
            IntVec payload({jobId});
            MyMpi::isend(_root_nodes[jobId], MSG_INCREMENTAL_JOB_FINISHED, payload);
            finishJob(jobId, /*hasIncrementalSuccessors=*/false);
        }
    }

    // Introduce next job(s) as applicable
    // (only one job at a time to react better
    // to outside events without too much latency)
    introduceNextJob();
    
    // Advance an all-reduction of the current system state
    if (_sys_state.aggregate(time)) {
        float* result = _sys_state.getGlobal();
        int processed = (int)result[SYSSTATE_PROCESSED_JOBS];
        int verb = (MyMpi::rank(_comm) == 0 ? V2_INFO : V5_DEBG);
        log(verb, "sysstate entered=%i parsed=%i scheduled=%i processed=%i\n", 
                    (int)result[SYSSTATE_ENTERED_JOBS], 
                    (int)result[SYSSTATE_PARSED_JOBS], 
                    (int)result[SYSSTATE_SCHEDULED_JOBS], 
                    processed);
        int jobLimit = _params.numJobs();
        if (jobLimit > 0 && processed >= jobLimit) {
            log(V2_INFO, "Job limit reached.\n");
            // Job limit reached - exit
            Terminator::setTerminating();
            // Send MSG_EXIT to worker of rank 0, which will broadcast it
            MyMpi::isend(0, MSG_DO_EXIT, IntVec({0}));
        }
    }
}

int Client::getMaxNumParallelJobs() {
    return _params.activeJobsPerClient();
}

void Client::introduceNextJob() {

    if (Terminator::isTerminating(/*fromMainThread=*/true)) 
        return;

    // Are there any non-introduced jobs left?
    if (_num_ready_jobs == 0) return;
    
    // To check if there is space for another active job in this client's "bucket"
    size_t lbc = getMaxNumParallelJobs();

    // Remove first eligible job from ready queue
    std::shared_ptr<JobDescription> jobPtr;
    {
        auto lock = _ready_job_lock.getLock();
        auto it = _ready_job_queue.begin(); 
        for (; it != _ready_job_queue.end(); ++it) {
            auto& j = *it;
            // Either there is still space for another job,
            // or the job must be incremental and already active
            if (lbc <= 0 || _active_jobs.size() < lbc || 
                (j->isIncremental() && _active_jobs.count(j->getId()))) {
                jobPtr = j;
                break;
            }
        }
        if (jobPtr) _ready_job_queue.erase(it);
    }
    if (!jobPtr) return;
    _num_ready_jobs--;

    // Store as an active job
    JobDescription& job = *jobPtr;
    int jobId = job.getId();
    _active_jobs[jobId] = jobPtr;
    _sys_state.addLocal(SYSSTATE_SCHEDULED_JOBS, 1);

    // Set actual job arrival
    float time = Timer::elapsedSeconds();
    job.setArrival(time);

    int nodeRank;
    if (job.isIncremental()) {
        // Incremental job: Send request to root node in standby
        nodeRank = _root_nodes[jobId];
    } else {
        // Find the job's canonical initial node
        int n = _params.numWorkers() >= 0 ? _params.numWorkers() : MyMpi::size(MPI_COMM_WORLD);
        log(V5_DEBG, "Creating permutation of size %i ...\n", n);
        AdjustablePermutation p(n, jobId);
        nodeRank = p.get(0);
    }

    JobRequest req(jobId, job.getApplication(), /*rootRank=*/-1, /*requestingNodeRank=*/_world_rank, 
        /*requestedNodeIndex=*/0, /*timeOfBirth=*/time, /*balancingEpoch=*/-1, /*numHops=*/0);
    req.revision = job.getRevision();
    req.timeOfBirth = job.getArrival();

    log(LOG_ADD_DESTRANK | V2_INFO, "Introducing job #%i rev. %i : %s", nodeRank, jobId, req.revision, req.toStr().c_str());
    MyMpi::isend(nodeRank, MSG_REQUEST_NODE, req);
}

void Client::handleOfferAdoption(MessageHandle& handle) {
    JobRequest req = Serializable::get<JobRequest>(handle.getRecvData());
    float schedulingTime = Timer::elapsedSeconds() - req.timeOfBirth;
    log(V3_VERB, "Scheduling %s on [%i] (latency: %.5fs)\n", req.toStr().c_str(), handle.source, schedulingTime);
    
    JobDescription& desc = *_active_jobs[req.jobId];
    desc.getStatistics().schedulingTime = schedulingTime;
    desc.getStatistics().timeOfScheduling = Timer::elapsedSeconds();
    assert(desc.getId() == req.jobId || log_return_false("%i != %i\n", desc.getId(), req.jobId));

    // Send job description
    log(LOG_ADD_DESTRANK | V4_VVER, "Sending job desc. of #%i rev. %i of size %i", handle.source, desc.getId(), 
        desc.getRevision(), desc.getTransferSize(desc.getRevision()));
    const auto& data = desc.getSerialization(desc.getRevision());
    int msgId = MyMpi::isend(handle.source, MSG_SEND_JOB_DESCRIPTION, data);
    log(LOG_ADD_DESTRANK | V4_VVER, "Sent job desc. of #%i of size %i", handle.source, req.jobId, data->size());
    
    // Remember transaction
    _root_nodes[req.jobId] = handle.source;
    _transfer_msg_id_to_job_id_rev[msgId] = std::pair<int, int>(req.jobId, desc.getRevision());
}

void Client::handleJobDone(MessageHandle& handle) {
    JobStatistics stats = Serializable::get<JobStatistics>(handle.getRecvData());
    log(LOG_ADD_SRCRANK | V4_VVER, "Will receive job result for job #%i", handle.source, stats.jobId);
    MyMpi::isendCopy(stats.successfulRank, MSG_QUERY_JOB_RESULT, handle.getRecvData());
    JobDescription& desc = *_active_jobs[stats.jobId];
    desc.getStatistics().usedWallclockSeconds = stats.usedWallclockSeconds;
    desc.getStatistics().usedCpuSeconds = stats.usedCpuSeconds;
}

void Client::handleSendJobResult(MessageHandle& handle) {

    JobResult jobResult = Serializable::get<JobResult>(handle.getRecvData());
    int jobId = jobResult.id;
    int resultCode = jobResult.result;
    int revision = jobResult.revision;

    log(LOG_ADD_SRCRANK | V4_VVER, "Received result of job #%i rev. %i, code: %i", handle.source, jobId, revision, resultCode);
    JobDescription& desc = *_active_jobs[jobId];
    desc.getStatistics().processingTime = Timer::elapsedSeconds() - desc.getStatistics().timeOfScheduling;

    // Output response time and solution header
    log(V2_INFO, "RESPONSE_TIME #%i %.6f rev. %i\n", jobId, Timer::elapsedSeconds()-desc.getArrival(), revision);
    log(V2_INFO, "SOLUTION #%i %s rev. %i\n", jobId, resultCode == RESULT_SAT ? "SAT" : "UNSAT", revision);

    std::string resultString = "s " + std::string(resultCode == RESULT_SAT ? "SATISFIABLE" 
                        : resultCode == RESULT_UNSAT ? "UNSATISFIABLE" : "UNKNOWN") + "\n";
    std::stringstream modelString;
    if ((_params.solutionToFile.isSet() || _params.monoFilename.isSet()) 
            && resultCode == RESULT_SAT) {
        modelString << "v ";
        for (size_t x = 1; x < jobResult.solution.size(); x++) {
            modelString << std::to_string(jobResult.solution[x]) << " ";
        }
        modelString << "0\n";
    }
    if (_params.solutionToFile.isSet()) {
        std::ofstream file;
        file.open(_params.solutionToFile());
        if (!file.is_open()) {
            log(V0_CRIT, "[ERROR] Could not open solution file\n");
        } else {
            file << resultString;
            file << modelString.str();
            file.close();
        }
    } else if (_params.monoFilename.isSet()) {
        log(LOG_NO_PREFIX | V0_CRIT, resultString.c_str());
        log(LOG_NO_PREFIX | V0_CRIT, modelString.str().c_str());
    }

    if (_file_adapter) {
        _file_adapter->handleJobDone(jobResult, desc.getStatistics());
    }

    finishJob(jobId, /*hasIncrementalSuccessors=*/_active_jobs[jobId]->isIncremental());
}

void Client::handleAbort(MessageHandle& handle) {

    IntVec request = Serializable::get<IntVec>(handle.getRecvData());
    int jobId = request[0];
    log(LOG_ADD_SRCRANK | V2_INFO, "TIMEOUT #%i %.6f", handle.source, jobId, 
            Timer::elapsedSeconds() - _active_jobs[jobId]->getArrival());
    
    if (_file_adapter) {
        JobResult result;
        result.id = jobId;
        result.revision = _active_jobs[result.id]->getRevision();
        result.result = 0;
        _file_adapter->handleJobDone(result, _active_jobs[result.id]->getStatistics());
    }

    finishJob(jobId, /*hasIncrementalSuccessors=*/_active_jobs[jobId]->isIncremental());
}

void Client::handleJobDescriptionSent(int msgId) {
    if (_transfer_msg_id_to_job_id_rev.count(msgId)) {
        const auto& [jobId, rev] = _transfer_msg_id_to_job_id_rev.at(msgId);
        if (_active_jobs.count(jobId)) {
            log(V4_VVER, "Clear description of #%i rev. %i\n", jobId, rev);
            _active_jobs.at(jobId)->clearPayload(rev);
            _num_loaded_jobs--;
        }
        _transfer_msg_id_to_job_id_rev.erase(msgId);
    }
}

void Client::finishJob(int jobId, bool hasIncrementalSuccessors) {

    // Clean up job, remember as done
    {
        auto lock = _done_job_lock.getLock();
        _done_jobs[jobId] = DoneInfo{_active_jobs[jobId]->getRevision(), _active_jobs[jobId]->getChecksum()};
    }
    if (!hasIncrementalSuccessors) {
        _root_nodes.erase(jobId);
        _active_jobs.erase(jobId);
        _sys_state.addLocal(SYSSTATE_PROCESSED_JOBS, 1);
    }

    introduceNextJob();
}

void Client::handleExit(MessageHandle& handle) {
    Terminator::setTerminating();
}

Client::~Client() {
    _instance_reader.stopWithoutWaiting();
    _incoming_job_cond_var.notify();
    _instance_reader.stop();
    _file_adapter.reset();

    // Merge logs from instance reader
    Logger::getMainInstance().mergeJobLogs(0);
    Logger::getMainInstance().mergeJobLogs(-1);

    log(V4_VVER, "Leaving client destructor\n");
}