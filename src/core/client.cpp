
#include <unistd.h>
#include <assert.h>
#include <fstream>
#include <string>
#include <list>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <utility>

#include "client.hpp"
#include "app/sat/data/model_string_compressor.hpp"
#include "comm/msg_queue/message_handle.hpp"
#include "comm/msgtags.h"
#include "data/job_interrupt_reason.hpp"
#include "interface/api/api_registry.hpp"
#include "util/string_utils.hpp"
#include "util/sys/timer.hpp"
#include "util/logger.hpp"
#include "util/permutation.hpp"
#include "util/sys/proc.hpp"
#include "data/job_transfer.hpp"
#include "data/job_result.hpp"
#include "app/sat/job/sat_constants.h"
#include "util/sys/terminator.hpp"
#include "util/sys/thread_pool.hpp"
#include "util/sys/atomics.hpp"
#include "app/app_registry.hpp"
#include "util/sys/tmpdir.hpp"
#include "util/sys/watchdog.hpp"
#include "interface/socket/socket_connector.hpp"
#include "interface/filesystem/naive_filesystem_connector.hpp"
#include "interface/filesystem/inotify_filesystem_connector.hpp"
#include "interface/api/api_connector.hpp"
#include "data/serializable.hpp"
#include "interface/api/job_id_allocator.hpp"
#include "interface/connector.hpp"
#include "interface/json_interface.hpp"
#include "util/json.hpp"
#include "util/sys/fileutils.hpp"


// Executed by a separate worker thread
void Client::readIncomingJobs() {

    Logger log = Logger::getMainInstance().copy("<Reader>", ".reader");
    LOGGER(log, V3_VERB, "Starting\n");

    std::vector<std::future<void>> taskFutures;
    std::set<std::pair<int, int>> unreadyJobs;

    while (true) {
        // Wait for a nonempty incoming job queue
        _incoming_job_cond_var.wait(_incoming_job_lock, [&]() {
            return !_instance_reader.continueRunning() 
                || (_num_incoming_jobs > 0 && _num_loaded_jobs < _params.loadedJobsPerClient());
        });
        if (!_instance_reader.continueRunning()) break;
        if (_num_loaded_jobs >= _params.loadedJobsPerClient()) continue;

        // Obtain lock, measure time
        auto lock = _incoming_job_lock.getLock();
        float time = Timer::elapsedSeconds();

        auto checkUnready = [&](const std::unique_ptr<JobDescription>& desc) {
            auto pair = std::pair<int, int>(desc->getId(), desc->getRevision());
            if (!unreadyJobs.count(pair)) {
                LOGGER(log, V2_INFO, "Deferring #%i rev. %i\n", pair.first, pair.second);
                unreadyJobs.insert(pair);
            }
        };

        // Find a single job eligible for parsing
        bool foundAJob = false;
        for (auto& data : _incoming_job_queue) {
            
            // Jobs are sorted by arrival:
            // If this job has not arrived yet, then none have arrived yet
            if (time < data.description->getArrival()) {
                checkUnready(data.description);
                continue;
            }

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
                }
            }
            if (!dependenciesSatisfied) {
                checkUnready(data.description);
                continue;
            }

            // Check if all job descriptions are already present
            bool jobDescriptionsPresent = true;
            for (auto& file : data.files) {
                if (!FileUtils::exists(file)) {
                    jobDescriptionsPresent = false;
                    break;
                }
            }
            if (!jobDescriptionsPresent) {
                // Print out waiting message every second
                if (time - data.lastDescriptionAvailabilityCheck >= 1) {
                    LOGGER(log, V2_INFO, "Waiting for a job description of #%i rev. %i\n", 
                        data.description->getId(),
                        data.description->getRevision());
                    data.lastDescriptionAvailabilityCheck = time;
                }
                continue;
            }

            // Job can be read: Enqueue reader task into thread pool
            LOGGER(log, V4_VVER, "ENQUEUE #%i\n", data.description->getId());
            unreadyJobs.erase(std::pair<int, int>(data.description->getId(), data.description->getRevision()));
            if (_params.monoFilename.isSet() && _mono_job_id < 0) _mono_job_id = data.description->getId();

            auto node = _incoming_job_queue.extract(data);
            auto future = ProcessWideThreadPool::get().addTask(
                [this, &log, foundJobPtr = new JobMetadata(std::move(node.value()))]() mutable {
                
                auto& foundJob = *foundJobPtr;
                if (!_instance_reader.continueRunning()) return;
                
                // Read job
                int id = foundJob.description->getId();
                float time = Timer::elapsedSeconds();
                bool success = true;
                auto filesList = foundJob.getFilesList();
                foundJob.description->beginInitialization(foundJob.description->getRevision());
                if (foundJob.hasFiles()) {
                    LOGGER(log, V3_VERB, "[T] Reading job #%i rev. %i %s ...\n", id, foundJob.description->getRevision(), filesList.c_str());
                    success = app_registry::getJobReader(foundJob.description->getApplicationId())(
                        _params, foundJob.files, *foundJob.description
                    );
                }
                foundJob.description->endInitialization();
                if (!success) {
                    LOGGER(log, V1_WARN, "[T] [WARN] Unsuccessful read - skipping #%i\n", id);
                    auto lock = _failed_job_lock.getLock();
                    _failed_job_queue.push_back(foundJob.jobName);
                    atomics::incrementRelaxed(_num_failed_jobs);
                } else {
                    time = Timer::elapsedSeconds() - time;
                    LOGGER(log, V3_VERB, "[T] Initialized job #%i %s in %.3fs: %ld lits w/ separators, %ld assumptions\n", 
                            id, filesList.c_str(), time, foundJob.description->getNumFormulaLiterals(), 
                            foundJob.description->getNumAssumptionLiterals());
                    foundJob.description->getStatistics().parseTime = time;

                    const int appId = foundJob.description->getApplicationId();
                    if (app_registry::isClientSide(appId)) {
                        // Launch client-side program
                        auto lock = _client_side_jobs_mutex.getLock();
                        _client_side_jobs.emplace_back(std::move(foundJob.description));
                        _sys_state.addLocal(SYSSTATE_SCHEDULED_JOBS, 1);
                        auto& clientSideJob = _client_side_jobs.back();
                        bool* done = &clientSideJob.done;
                        JobResult* res = &clientSideJob.result;
                        auto* desc = clientSideJob.desc.get();
                        // store "original" arrival time value as the job's submission time
                        desc->getStatistics().timeOfSubmission = desc->getArrival();
                        desc->getStatistics().timeOfScheduling = Timer::elapsedSeconds();
                        clientSideJob.program.reset(
                            app_registry::getClientSideProgramCreator(appId)(_params, getAPI(), *desc)
                        );
                        clientSideJob.thread->run([&, prog = clientSideJob.program.get(), done, res]() {
                            *res = prog->function();
                            *done = true;
                        });
                    } else {
                        // Enqueue in ready jobs to be scheduled properly
                        auto lock = _ready_job_lock.getLock();
                        _ready_job_queue.push_back(std::move(foundJob.description));
                        atomics::incrementRelaxed(_num_ready_jobs);
                    }
                    atomics::incrementRelaxed(_num_loaded_jobs);
                    _sys_state.addLocal(SYSSTATE_PARSED_JOBS, 1);
                }

                delete foundJobPtr;
            });
            taskFutures.push_back(std::move(future));
            foundAJob = true;
            break;
        }

        if (foundAJob) {
            _num_incoming_jobs--;
        }
    }

    LOGGER(log, V3_VERB, "Stopping\n");
    // Wait for all tasks to finish
    for (auto& future : taskFutures) future.get();
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
    if (data.interrupt) {
        // Interrupt job (-> abort entire job if non-incremental, abort iteration if incremental)
        int jobId = data.description->getId();
        int rev = data.description->getRevision();
        {
            auto lock = _jobs_to_interrupt_lock.getLock();
            _jobs_to_interrupt.push_back({jobId, rev});
        }
        atomics::incrementRelaxed(_num_jobs_to_interrupt);
        return;
    }

    // Introduce new job into "incoming" queue
    data.description->setClientRank(_world_rank);
    {
        auto lock = _arrival_times_lock.getLock();
        _arrival_times.insert(data.description->getArrival());
        _next_arrival_time_millis.store(1000 * *_arrival_times.begin(), std::memory_order_relaxed);
    }
    {
        auto lock = _incoming_job_lock.getLock();
        _incoming_job_queue.insert(std::move(data));
        _num_incoming_jobs++;
    }
    _incoming_job_cond_var.notify();
    _sys_state.addLocal(SYSSTATE_ENTERED_JOBS, 1);
}

void Client::init() {

    // Get ID allocator this client should use
    JobIdAllocator jobIdAllocator(MyMpi::rank(_comm), MyMpi::size(_comm), getFilesystemInterfacePath());

    // Set up generic JSON interface to communicate with this client
    _json_interface = std::unique_ptr<JsonInterface>(
        new JsonInterface(getInternalRank(), _params, 
            Logger::getMainInstance().copy("I", ".i"),
            [&](JobMetadata&& data) {handleNewJob(std::move(data));},
            std::move(jobIdAllocator)
        )
    );

    // Set up various interfaces as bridges between the outside and the JSON interface
    if (_params.useFilesystemInterface()) {
        std::string path = getFilesystemInterfacePath();
        {
            // Write the available job submission path to an availability tmp file
            std::ofstream ofs(TmpDir::getGeneralTmpDir() + "/edu.kit.iti.mallob.apipath." + std::to_string(Proc::getPid()));
            // Differentiate absolute vs. relative path
            if (path[0] == '/') ofs << path;
            else ofs << std::filesystem::current_path().string() + "/" + path;
        }
        LOG(V2_INFO, "Set up filesystem interface at %s\n", path.c_str());
        // Tell JSON interface to output non-JSON result files to the interface path, too
        _json_interface->setOutputDirectory(path);

        auto logger = Logger::getMainInstance().copy("I-FS", ".i-fs");
        auto conn = _params.inotify() ? 
            (Connector*) new InotifyFilesystemConnector(*_json_interface, _params, std::move(logger), path)
            :
            (Connector*) new NaiveFilesystemConnector(*_json_interface, _params, std::move(logger), path);
        _interface_connectors.emplace_back(conn);
    }
    if (_params.useIPCSocketInterface()) {
        std::string path = getSocketPath();
        LOG(V2_INFO, "Set up IPC socket interface at %s\n", path.c_str());
        _interface_connectors.emplace_back(new SocketConnector(_params, *_json_interface, path));
    }
    _api_connector.reset(new APIConnector(*_json_interface, _params, Logger::getMainInstance().copy("I-API", ".i.api")));
    APIRegistry::put(_api_connector);
    _interface_connectors.emplace_back(_api_connector);
    LOG(V2_INFO, "Set up API at %s\n", "src/interface/api/api_connector.hpp");

    // Set up concurrent instance reader
    _instance_reader.run([this]() {
        Proc::nameThisThread("InstanceReader");
        readIncomingJobs();
    });

    // Set up callbacks for client-specific MPI messages
    _subscriptions.emplace_back(MSG_NOTIFY_JOB_DONE, [&](auto& h) {handleJobDone(h);});
    _subscriptions.emplace_back(MSG_SEND_JOB_RESULT, [&](auto& h) {handleSendJobResult(h);});
    _subscriptions.emplace_back(MSG_NOTIFY_CLIENT_JOB_ABORTING, [&](auto& h) {handleAbort(h);});
    _subscriptions.emplace_back(MSG_OFFER_ADOPTION_OF_ROOT, [&](auto& h) {handleOfferAdoption(h);});
}

int Client::getInternalRank() {
    return MyMpi::rank(_comm) + _params.firstApiIndex();
}

std::string Client::getFilesystemInterfacePath() {
    return _params.apiDirectory() + "/jobs." + std::to_string(getInternalRank()) + "/";
}

std::string Client::getSocketPath() {
    return TmpDir::getGeneralTmpDir() + "/edu.kit.iti.mallob." + std::to_string(Proc::getPid()) + "." + std::to_string(getInternalRank()) + ".sk";
} 

APIConnector& Client::getAPI() {
    return *_api_connector;
}

void Client::advance() {
    
    auto time = Timer::elapsedSecondsCached();

    // Send notification messages for recently done jobs
    if (_periodic_check_done_jobs.ready(time)) {
        robin_hood::unordered_flat_set<int, robin_hood::hash<int>> doneJobs;
        {
            auto lock = _done_job_lock.getLock();
            doneJobs = std::move(_recently_done_jobs);
            _recently_done_jobs.clear();
        }
        for (int jobId : doneJobs) {
            LOG_ADD_DEST(V3_VERB, "Notify #%i:0 that job is done", _root_nodes[jobId], jobId);
            IntVec payload({jobId});
            MyMpi::isend(_root_nodes[jobId], MSG_INCREMENTAL_JOB_FINISHED, payload);
            finishJob(jobId, /*hasIncrementalSuccessors=*/false);
        }
    }

    // Check if any client-side jobs are done
    if (_periodic_check_client_side_jobs.ready(time) && _client_side_jobs_mutex.tryLock()) {
        for (auto it = _client_side_jobs.begin(); it != _client_side_jobs.end(); ++it) {
            auto& job = *it;
            if (!job.done) continue;
            job.thread->join();
            const int id = job.desc->getId();
            MyMpi::isend(_world_rank, MSG_SEND_JOB_RESULT, job.result);
            _done_client_side_jobs.push_back(std::move(job));
            it = _client_side_jobs.erase(it);
            --it;
        }
        _client_side_jobs_mutex.unlock();
    }

    if (_num_jobs_to_interrupt.load(std::memory_order_relaxed) > 0 && _jobs_to_interrupt_lock.tryLock()) {
        
        auto it = _jobs_to_interrupt.begin();
        while (it != _jobs_to_interrupt.end()) {
            auto [jobId, rev] = *it;
            if (_done_jobs.count(jobId) && _done_jobs[jobId].revision >= rev) {
                LOG(V2_INFO, "Interrupt #%i obsolete\n", jobId);
                it = _jobs_to_interrupt.erase(it);
                atomics::decrementRelaxed(_num_jobs_to_interrupt);
            } else if (rev > (_active_jobs.count(jobId) ? _active_jobs[jobId]->getRevision() : -1)) {
                ++it;
            } else if (_root_nodes.count(jobId)) {
                LOG(V2_INFO, "Interrupt #%i\n", jobId);
                MyMpi::isend(_root_nodes.at(jobId), MSG_NOTIFY_JOB_ABORTING, IntVec({jobId, rev, JobInterruptReason::USER}));
                it = _jobs_to_interrupt.erase(it);
                atomics::decrementRelaxed(_num_jobs_to_interrupt);
            } else ++it;
        }
        _jobs_to_interrupt_lock.unlock();
    }

    if (_num_failed_jobs.load(std::memory_order_relaxed) > 0 && _failed_job_lock.tryLock()) {
        for (auto& jobname : _failed_job_queue) {
            LOG(V1_WARN, "[WARN] Rejecting submission %s - reason: Error while parsing description.\n", jobname.c_str());
            if (_params.monoFilename.isSet()) {
                MyMpi::broadcastExitSignal();
            }
        }
        _failed_job_queue.clear();
        _failed_job_lock.unlock();
    }

    // Process arrival times chronologically; if at least one "happened", notify reader
    auto nextArrival = 0.001f * _next_arrival_time_millis.load(std::memory_order_relaxed);
    if (nextArrival >= 0 && time >= nextArrival && _arrival_times_lock.tryLock()) {
        bool notify = false;
        _next_arrival_time_millis.store(-1, std::memory_order_relaxed);
        for (auto it = _arrival_times.begin(); it != _arrival_times.end(); ++it) {
            float arr = *it;
            if (arr > time) {
                _next_arrival_time_millis.store(1000*arr, std::memory_order_relaxed);
                break;
            }
            
            notify = true;
            it = _arrival_times.erase(it);
            if (it == _arrival_times.end()) break;
            --it;
        }
        _arrival_times_lock.unlock();
        if (notify) {
            {auto lock = _incoming_job_lock.getLock();}
            _incoming_job_cond_var.notify();
        }
    }

    // Introduce next job(s) as applicable
    // (only one job at a time to react better
    // to outside events without too much latency)
    introduceNextJob();
    
    // Advance an all-reduction of the current system state
    if (_sys_state.aggregate(time)) {
        const std::vector<float>& result = _sys_state.getGlobal();
        if (MyMpi::rank(_comm) == 0) {
            LOG(V2_INFO, "sysstate entered=%i parsed=%i scheduled=%i processed=%i successful=%i\n",
                (int)result[SYSSTATE_ENTERED_JOBS], 
                (int)result[SYSSTATE_PARSED_JOBS], 
                (int)result[SYSSTATE_SCHEDULED_JOBS],
                (int)result[SYSSTATE_PROCESSED_JOBS],
                (int)result[SYSSTATE_SUCCESSFUL_JOBS]);
        }
    }

    // Any "done job" processings still pending?
    if (_pending_subtasks.empty()) {
        // -- no: check if job limit has been reached

        bool jobLimitReached = false;
        int jobLimit = _params.numJobs();
        if (jobLimit > 0) jobLimitReached |= (int)_sys_state.getGlobal()[SYSSTATE_PROCESSED_JOBS] >= jobLimit;
        int successfulJobLimit = _params.numSuccessfulJobs();
        if (successfulJobLimit > 0)
            jobLimitReached |= (int)_sys_state.getGlobal()[SYSSTATE_SUCCESSFUL_JOBS] >= successfulJobLimit;

        if (jobLimitReached) {
            LOG(V2_INFO, "Job limit reached.\n");
            // Job limit reached - exit
            Terminator::setTerminating();
            // Send MSG_EXIT to worker of rank 0, which will broadcast it
            MyMpi::broadcastExitSignal();
            // Stop instance reader immediately
            _instance_reader.stopWithoutWaiting();
        }
    } else {
        // processings pending - try to join done tasks
        while (!_pending_subtasks.empty() && _pending_subtasks.front().done) {
            _pending_subtasks.front().future.get();
            _pending_subtasks.pop_front();
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
    if (_num_ready_jobs.load(std::memory_order_relaxed) == 0) return;
    
    // To check if there is space for another active job in this client's "bucket"
    size_t lbc = getMaxNumParallelJobs();

    // Remove first eligible job from ready queue
    std::unique_ptr<JobDescription> jobPtr;
    {
        auto lock = _ready_job_lock.getLock();
        auto it = _ready_job_queue.begin(); 
        for (; it != _ready_job_queue.end(); ++it) {
            auto& j = *it;
            // Either there is still space for another job,
            // or the job must be incremental and already active
            if (lbc <= 0 || _active_jobs.size() < lbc || 
                (j->isIncremental() && _active_jobs.count(j->getId()))) {
                jobPtr = std::move(j);
                break;
            }
        }
        if (jobPtr) _ready_job_queue.erase(it);
    }
    if (!jobPtr) return;
    atomics::decrementRelaxed(_num_ready_jobs);

    // Store as an active job
    JobDescription& job = *jobPtr;
    int jobId = job.getId();
    _active_jobs[jobId] = std::move(jobPtr);
    _sys_state.addLocal(SYSSTATE_SCHEDULED_JOBS, 1);

    // store "original" arrival time value as the job's submission time
    job.getStatistics().timeOfSubmission = job.getArrival();
    // Set actual job "arrival" in terms of Mallob's scheduling
    float time = Timer::elapsedSeconds();
    job.setArrival(time);

    int nodeRank;
    if (job.isIncremental() && _root_nodes.count(jobId)) {
        // Incremental job: Send request to root node in standby
        nodeRank = _root_nodes[jobId];
    } else {
        if (_params.monoFilename.isSet()) {
            // Mono instance mode: Use rank 0 as the job's root
            nodeRank = 0;
        } else {
            // Find the job's canonical initial node
            int n = _params.numWorkers() >= 0 ? _params.numWorkers() : MyMpi::size(MPI_COMM_WORLD);
            LOG(V5_DEBG, "Creating permutation of size %i ...\n", n);
            AdjustablePermutation p(n, jobId);
            nodeRank = p.get(0);
        }
    }

    JobRequest req(jobId, job.getApplicationId(), /*rootRank=*/-1, /*requestingNodeRank=*/_world_rank, 
        /*requestedNodeIndex=*/0, /*timeOfBirth=*/time, /*balancingEpoch=*/-1, /*numHops=*/0, job.isIncremental());
    req.revision = job.getRevision();
    req.timeOfBirth = job.getArrival();

    LOG_ADD_DEST(V2_INFO, "Introducing job #%i rev. %i : %s", nodeRank, jobId, req.revision, req.toStr().c_str());
    if (job.isIncremental() && req.revision > 0) {
        sendJobDescription(req, nodeRank);
    } else {
        MyMpi::isend(nodeRank, MSG_REQUEST_NODE, req);
    }
}

void Client::handleOfferAdoption(MessageHandle& handle) {
    JobRequest req = Serializable::get<JobRequest>(handle.getRecvData());
    sendJobDescription(req, handle.source);
}

void Client::sendJobDescription(JobRequest& req, int destRank) {

    float schedulingTime = Timer::elapsedSeconds() - req.timeOfBirth;
    LOG(V3_VERB, "Scheduling %s on [%i] (latency: %.5fs)\n", req.toStr().c_str(), destRank, schedulingTime);

    JobDescription& desc = *_active_jobs[req.jobId];
    desc.getStatistics().schedulingTime = schedulingTime;
    desc.getStatistics().timeOfScheduling = Timer::elapsedSeconds();
    assert(desc.getId() == req.jobId || LOG_RETURN_FALSE("%i != %i\n", desc.getId(), req.jobId));

    // Send job description
    LOG_ADD_DEST(V4_VVER, "Sending job desc. of #%i rev. %i of size %lu, formula begins at idx %lu", destRank, desc.getId(),
        desc.getRevision(), desc.getTransferSize(desc.getRevision()),
        ((const uint8_t*)desc.getFormulaPayload(desc.getRevision()) - desc.getRevisionData(desc.getRevision())->data()));

    int tag = desc.isIncremental() && req.revision>0 ?
        MSG_DEPLOY_NEW_REVISION : MSG_SEND_JOB_DESCRIPTION;

    auto data = desc.getSerialization(desc.getRevision());

    log(V5_DEBG, "Set up formula : %s\n",
        StringUtils::getSummary((const int*) desc.getFormulaPayload(desc.getRevision()),
        desc.getFormulaPayloadSize(desc.getRevision())).c_str());

    desc.clearPayload(desc.getRevision());
    int msgId = MyMpi::isend(destRank, tag, data);
    LOG_ADD_DEST(V4_VVER, "Sent job desc. of #%i of size %lu", 
        destRank, req.jobId, data->size());
    //LOG(V4_VVER, "%p : use count %i\n", data.get(), data.use_count());

    // Remember transaction
    _root_nodes[req.jobId] = destRank;

    // waiting instance reader might be able to continue now
    {
        auto lock = _incoming_job_lock.getLock();
        _num_loaded_jobs--;
    }
    _incoming_job_cond_var.notify(); 
}

void Client::handleJobDone(MessageHandle& handle) {
    JobStatistics stats = Serializable::get<JobStatistics>(handle.getRecvData());

    if (!_active_jobs.count(stats.jobId)) return; // user-side terminated in the meantime?
    JobDescription& desc = *_active_jobs[stats.jobId];
    if (desc.getRevision() > stats.revision) return; // revision obsolete!
    LOG_ADD_SRC(V4_VVER, "Will receive job result for job #%i rev. %i", handle.source, stats.jobId, stats.revision);
    MyMpi::isendCopy(stats.successfulRank, MSG_QUERY_JOB_RESULT, handle.getRecvData());
    desc.getStatistics().usedWallclockSeconds = stats.usedWallclockSeconds;
    desc.getStatistics().usedCpuSeconds = stats.usedCpuSeconds;
    desc.getStatistics().latencyOf1stVolumeUpdate = stats.latencyOf1stVolumeUpdate;
}

void Client::handleSendJobResult(MessageHandle& handle) {

    JobResult jobResult(handle.moveRecvData());
    int& jobId = jobResult.id;
    int resultCode = jobResult.result;
    int revision = jobResult.revision;

    JobDescription* descPtr = getActiveJob(jobId);
    if (!descPtr) return; // user-side terminated in the meantime?
    JobDescription& desc = *descPtr;
    if (desc.getRevision() > revision) return; // revision obsolete!
    int surrogateId = desc.getAppConfiguration().map.count("__surrogate") ?
        desc.getAppConfiguration().fixedSizeEntryToInt("__surrogate") : 0;
    float timeOfParentTask = 0;
    if (surrogateId != 0) {
        LOG_ADD_SRC(V4_VVER, "Result of job #%i is surrogate for #%i", handle.source, jobId, surrogateId);
        jobId = surrogateId;
        // need to add time spent by parent task
        timeOfParentTask = atof(desc.getAppConfiguration().map["__spentwcsecs"].c_str());
    }

    LOG_ADD_SRC(V4_VVER, "Received result of job #%i rev. %i, code: %i", handle.source, jobId, revision, resultCode);
    const float now = Timer::elapsedSeconds();
    desc.getStatistics().processingTime = now - desc.getStatistics().timeOfScheduling + timeOfParentTask;
    desc.getStatistics().totalResponseTime = now - desc.getStatistics().timeOfSubmission + timeOfParentTask;

    std::string resultCodeString = "UNKNOWN";
    if (resultCode == RESULT_SAT) resultCodeString = "SATISFIABLE";
    if (resultCode == RESULT_UNSAT) resultCodeString = "UNSATISFIABLE";
    if (resultCode == RESULT_OPTIMUM_FOUND) resultCodeString = "OPTIMUM FOUND";

    // Output response time and solution header
    LOG(V2_INFO, "RESPONSE_TIME #%i %.6f rev. %i\n", jobId, desc.getStatistics().totalResponseTime, revision);
    LOG(V2_INFO, "SOLUTION #%i %s rev. %i\n", jobId, resultCodeString.c_str(), revision);

    // Increment # successful jobs if result is not "unknown"
    if (resultCode != 0) {
        _sys_state.addLocal(SYSSTATE_SUCCESSFUL_JOBS, 1);
    }

    std::string resultString = "s " + resultCodeString + "\n";
    std::vector<std::string> modelStrings;
    // Decide whether we need to construct a string representing a solution.
    // - In "mono" mode of operation, we only want the original job, not a secondary one.
    bool primaryJob = !_params.monoFilename.isSet() || jobId == _mono_job_id;
    bool constructSolutionStrings = primaryJob;
    // - Only if the result actually encompasses a solution.
    constructSolutionStrings &= resultCode == RESULT_SAT || resultCode == RESULT_OPTIMUM_FOUND;
    // - Some sort of output is in fact desired by the user.
    constructSolutionStrings &= _params.solutionToFile.isSet() || (jobId == _mono_job_id && !_params.omitSolution());
    if (constructSolutionStrings) {

        // Disable all watchdogs to avoid crashes while printing a huge model
        // if (jobResult.getSolutionSize() > 1'000'000)
        //     Watchdog::disableGlobally();

        auto json = app_registry::getJobSolutionFormatter(desc.getApplicationId())(
            _params, jobResult, desc.getStatistics());
        if (json.is_array() && (json.size()==0 || json[0].is_string())) {
            auto jsonArr = json.get<std::vector<std::string>>();
            for (auto&& str : jsonArr) modelStrings.push_back(std::move(str));
        } else if (json.is_string()) {
            if (resultCode == RESULT_SAT && _params.compressModels()) {
                auto vec = ModelStringCompressor::decompress(json.get<std::string>());
                std::string modelStr = "v ";
                for (int l : vec) if (l!=0) modelStr += std::to_string(l) + " ";
                modelStr += "0\n";
                modelStrings.push_back(std::move(modelStr));
            } else {
                modelStrings.push_back(json.get<std::string>());
            }
        } else {
            modelStrings.push_back(json.dump()+"\n");
        }
    }
    if (constructSolutionStrings && _params.solutionToFile.isSet()) {
        // Write solution to file
        std::ofstream file;
        file.open(_params.solutionToFile() + "." + std::to_string(jobId) + "." + std::to_string(revision), std::ofstream::out);
        if (!file.is_open()) {
            LOG(V0_CRIT, "[ERROR] Could not open solution file\n");
        } else {
            file << resultString;
            for (auto& modelString : modelStrings) file << modelString;
            file.close();
        }
    }
    if (jobId == _mono_job_id) {
        // Mono job: log solution to stdout, write result hint if doing proof production
        LOG_OMIT_PREFIX(V0_CRIT, resultString.c_str());
        if (constructSolutionStrings && !_params.solutionToFile.isSet()) {
            for (auto& modelString : modelStrings)
                LOG_OMIT_PREFIX(V0_CRIT, modelString.c_str());
        }
        if (_params.proofOutputFile.isSet()) {
            std::ofstream resultFile(".mallob_result");
            std::string resultCodeStr = std::to_string(resultCode);
            if (resultFile.is_open()) resultFile.write(resultCodeStr.c_str(), resultCodeStr.size());
        }
    }

    int permanentId = jobId; // need to copy since jobId can get invalidated below
    if (_json_interface) {
        JobResult* resultPtr = new JobResult(std::move(jobResult));
        _pending_subtasks.emplace_back();
        bool* futFinished = &_pending_subtasks.back().done;
        _pending_subtasks.back().future = ProcessWideThreadPool::get().addTask(
            [interface = _json_interface.get(), 
            result = resultPtr, 
            stats = desc.getStatistics(),
            futFinished,
            applicationId = desc.getApplicationId()]() mutable {
            
            interface->handleJobDone(std::move(*result), stats, applicationId);
            delete result;
            *futFinished = true;
        });
    }

    Logger::getMainInstance().flush();
    finishJob(permanentId, /*hasIncrementalSuccessors=*/desc.isIncremental());
}

void Client::handleAbort(MessageHandle& handle) {

    IntVec request = Serializable::get<IntVec>(handle.getRecvData());
    int jobId = request[0];
    int rev = request[1];
    auto desc = getActiveJob(jobId);
    if (rev < desc->getRevision()) return;

    int surrogateId = desc->getAppConfiguration().map.count("__surrogate") ?
        desc->getAppConfiguration().fixedSizeEntryToInt("__surrogate") : 0;
    if (surrogateId != 0) {
        LOG_ADD_SRC(V4_VVER, "Result of job #%i is surrogate for #%i", handle.source, jobId, surrogateId);
        jobId = surrogateId;
    }

    LOG_ADD_SRC(V2_INFO, "TIMEOUT/UNKNOWN #%i rev. %i %.6f", handle.source, jobId,
        rev, Timer::elapsedSeconds() - desc->getArrival());

    if (_json_interface) {
        JobResult result;
        result.id = jobId;
        result.revision = rev;
        result.result = 0;
        _json_interface->handleJobDone(std::move(result), desc->getStatistics(),
            desc->getApplicationId());
    }

    finishJob(jobId, /*hasIncrementalSuccessors=*/desc->isIncremental());
}

void Client::finishJob(int jobId, bool hasIncrementalSuccessors) {

    if (!_active_jobs.count(jobId)) {
        // try to fetch client-side job
        for (auto it = _done_client_side_jobs.begin(); it != _done_client_side_jobs.end(); ++it) {
            auto& j = *it;
            if (j.desc->getId() != jobId) continue;
            // done client-side job found
            auto lock = _done_job_lock.getLock();
            _done_jobs[jobId] = DoneInfo{j.desc->getRevision(), j.desc->getChecksum()};
            if (hasIncrementalSuccessors) break;
            // delete resources (ClientSideJob and its JobDescription) concurrently
            _pending_subtasks.emplace_back();
            bool* futDone = &_pending_subtasks.back().done;
            _pending_subtasks.back().future = ProcessWideThreadPool::get().addTask(
                [&, futDone, j = new ClientSideJob(std::move(j))]() {
                delete j;
                *futDone = true;
            });
            _done_client_side_jobs.erase(it);
            break;
        }
    } else {
        auto lock = _done_job_lock.getLock();
        _done_jobs[jobId] = DoneInfo{_active_jobs[jobId]->getRevision(), _active_jobs[jobId]->getChecksum()};
    }

    // Clean up job, remember as done
    if (!hasIncrementalSuccessors) {
        _root_nodes.erase(jobId);
        _active_jobs.erase(jobId);
        _sys_state.addLocal(SYSSTATE_PROCESSED_JOBS, 1);
    }

    _incoming_job_cond_var.notify(); // waiting instance reader might be able to continue now
}

JobDescription* Client::getActiveJob(int jobId) {
    if (_active_jobs.count(jobId)) return _active_jobs[jobId].get();
    for (auto& j : _done_client_side_jobs) if (j.desc->getId() == jobId) return j.desc.get();
    return nullptr;
}

Client::~Client() {

    Watchdog watchdog(_params.watchdog(), 1'000, true);
    watchdog.setWarningPeriod(1'000);
    watchdog.setAbortPeriod(20'000);

    for (auto& pending : _pending_subtasks) pending.future.get();

    if (_json_interface) _json_interface->deactivate();

    // poke instance reader to notice termination
    _instance_reader.stopWithoutWaiting();
    {auto lock = _incoming_job_lock.getLock();}
    _incoming_job_cond_var.notify();
    _instance_reader.stop();

    for (auto& j : _client_side_jobs) j.thread->join();
    _client_side_jobs.clear();
    _done_client_side_jobs.clear();

    LOG(V4_VVER, "Leaving client destructor\n");
}