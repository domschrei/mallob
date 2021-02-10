
#include <iostream>
#include <fstream>
#include <string>
#include <chrono>
#include <thread>
#include <unistd.h>

#include "client.hpp"
#include "util/sat_reader.hpp"
#include "util/sys/timer.hpp"
#include "util/logger.hpp"
#include "util/permutation.hpp"
#include "util/sys/proc.hpp"
#include "data/job_transfer.hpp"
#include "data/job_result.hpp"
#include "util/random.hpp"
#include "app/sat/sat_constants.h"
#include "util/sys/terminator.hpp"

// Executed by a separate worker thread
void Client::readAllInstances() {

    log(V3_VERB, "FILE_IO started\n");

    for (size_t i = 0; i < _ordered_job_ids.size(); i++) {

        if (checkTerminate()) {
            log(V3_VERB, "FILE_IO stopping\n");
            return;
        }

        // Keep at most 10 full jobs in memory at any time 
        while (i - _last_introduced_job_idx > 10) {
            if (checkTerminate()) {
                log(V3_VERB, "FILE_IO stopping\n");
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        int jobId = _ordered_job_ids[i];
        JobDescription& job = *_jobs[jobId];
        if (job.isIncremental()) {
            log(V4_VVER, "FILE_IO Job #%i is incremental\n", jobId);
        }

        log(V3_VERB, "FILE_IO reading \"%s\" (#%i)\n", _job_instances[jobId].c_str(), jobId);

        if (job.isIncremental()) {
            int revision = 0;
            while (true) {
                std::fstream file;
                std::string filename = _job_instances[jobId] + "." + std::to_string(revision);
                file.open(filename, std::ios::in);
                if (file.is_open()) {
                    readFormula(filename, job);
                    job.setRevision(revision);
                } else {
                    break;
                }
                revision++;
            }
        } else {
            readFormula(_job_instances[jobId], job);
        }

        log(V3_VERB, "FILE_IO read \"%s\" (#%i)\n", _job_instances[jobId].c_str(), jobId);

        auto lock = _job_ready_lock.getLock();
        _job_ready[jobId] = true;
    }
}

void Client::handleNewJob(std::shared_ptr<JobDescription> desc) {
    auto lock = _incoming_job_queue_lock.getLock();
    _incoming_job_queue.push_back(desc);
}

void Client::init() {

    _last_introduced_job_idx = -1;
    int internalRank = MyMpi::rank(_comm);

    if (_params.isSet("scenario")) {
        std::string filename = _params.getParam("scenario") + "." + std::to_string(internalRank);
        readInstanceList(filename);
        _instance_reader_thread = std::thread(&Client::readAllInstances, this);
    } else {
        _file_adapter = std::unique_ptr<JobFileAdapter>(
            new JobFileAdapter(_params, 
                Logger::getMainInstance().copy("API", "#0."),
                ".api/jobs." + std::to_string(internalRank) + "/", 
                [&](std::shared_ptr<JobDescription> desc) {handleNewJob(desc);}
            )
        );
    }
    log(V2_INFO, "Client main thread started\n");

    // Begin listening to incoming messages
    MyMpi::beginListening();

    log(V4_VVER, "Global init barrier ...\n");
    MPI_Barrier(MPI_COMM_WORLD);
    log(V4_VVER, "Passed global init barrier\n");
}

bool Client::checkTerminate() {

    if (Terminator::isTerminating()) {
        log(V2_INFO, "Terminating.\n");
        return true;
    }
    if (Timer::globalTimelimReached(_params)) {
        log(V2_INFO, "Global timeout: terminating\n");
        Terminator::setTerminating();
        return true;
    }
    if (_num_alive_clients == 0) {
        // Send exit message to part of workers
        log(V3_VERB, "Clients done: sending EXIT to workers\n");

        // Send MSG_EXIT to worker of rank 0, which will broadcast it
        MyMpi::isend(MPI_COMM_WORLD, 0, MSG_DO_EXIT, IntVec({0}));

        // Force send all handles before exiting
        while (MyMpi::hasOpenSentHandles()) MyMpi::testSentHandles();
        Terminator::setTerminating();
        return true;
    }
    return false;
}

void Client::mainProgram() {

    float lastStatTime = Timer::elapsedSeconds();

    while (!checkTerminate()) {

        float time = Timer::elapsedSeconds();

        // Print memory usage info
        if (time - lastStatTime > 5) {
            auto info = Proc::getRuntimeInfo(Proc::getPid(), Proc::SubprocessMode::FLAT);
            info.vmUsage *= 0.001 * 0.001;
            info.residentSetSize *= 0.001 * 0.001;
            log(V4_VVER, "mainthread_cpu=%i\n", info.cpu);
            log(V3_VERB, "mem=%.2fGB\n", info.residentSetSize);
            lastStatTime = time;
        }

        // Introduce next job(s) as applicable
        // (only one job at a time to react better
        // to outside events without too much latency)
        int nextId = getNextIntroduceableJob();
        if (nextId >= 0) introduceJob(_jobs[nextId]);

        // Poll messages, if present
        auto maybeHandle = MyMpi::poll(time);
        if (maybeHandle) {
            // Process message
            auto& handle = *maybeHandle;
            log(LOG_ADD_SRCRANK | V5_DEBG, "process msg tag=%i", handle.source, handle.tag);

            if (handle.tag == MSG_NOTIFY_JOB_DONE) {
                handleJobDone(handle);
            } else if (handle.tag == MSG_SEND_JOB_RESULT) {
                handleSendJobResult(handle);
            } else if (handle.tag == MSG_NOTIFY_JOB_ABORTING) {
                handleAbort(handle);
            } else if (handle.tag == MSG_OFFER_ADOPTION) {
                handleRequestBecomeChild(handle);
            } else if (handle.tag == MSG_CONFIRM_ADOPTION) {
                handleAckAcceptBecomeChild(handle);
            } else if (handle.tag == MSG_QUERY_JOB_REVISION_DETAILS) {
                handleQueryJobRevisionDetails(handle);
            } else if (handle.tag == MSG_CONFIRM_JOB_REVISION_DETAILS) {
                handleAckJobRevisionDetails(handle);
            } else if (handle.tag == MSG_CLIENT_FINISHED) {
                handleClientFinished(handle);
            }  else if (handle.tag == MSG_DO_EXIT) {
                handleExit(handle);
            } else {
                log(LOG_ADD_SRCRANK | V1_WARN, "Unknown msg tag %i", handle.source, handle.tag);
            }
        }

        MyMpi::testSentHandles();

        // Sleep for a bit
        usleep(1000); // 1000 = 1 millisecond
    }

    Logger::getMainInstance().flush();
    fflush(stdout);
}

int Client::getMaxNumParallelJobs() {
    std::string query = "lbc" + std::to_string(MyMpi::rank(_comm));
    return _params.getIntParam(query.c_str());
}

int Client::getNextIntroduceableJob() {
    
    if (checkTerminate()) return -1;

    // Jobs in the incoming queue?
    {
        auto lock = _incoming_job_queue_lock.getLock();
        for (auto job : _incoming_job_queue) {
            int id = job->getId();
            _ordered_job_ids.push_back(id);
            _jobs[id] = job;
            assert(job->getPriority() > 0 && job->getPriority() <= 1.0f);

            auto lock = _job_ready_lock.getLock();
            _job_ready[id] = true;
        }
        _incoming_job_queue.clear();
    }

    // Are there any non-introduced jobs left?
    if (_last_introduced_job_idx+1 >= (int)_ordered_job_ids.size()) return -1;
    
    // -- yes
    int jobId = _ordered_job_ids[_last_introduced_job_idx+1];
    bool introduce = true;
    
    // Check if there is space for another active job in this client's "bucket"
    size_t lbc = getMaxNumParallelJobs();
    if (lbc > 0) introduce &= _introduced_job_ids.size() < lbc;
    
    // Check if job has already arrived
    introduce &= (_jobs[jobId]->getArrival() <= Timer::elapsedSeconds());
    
    // Check if job was already read
    introduce &= isJobReady(jobId);
    
    return introduce ? jobId : -1;
}

bool Client::isJobReady(int jobId) {
    auto lock = _job_ready_lock.getLock();
    return _job_ready.count(jobId) && _job_ready[jobId];
}

void Client::introduceJob(std::shared_ptr<JobDescription>& jobPtr) {

    JobDescription& job = *jobPtr;
    int jobId = job.getId();

    // Wait until job is ready to be sent
    while (true) {
        if (isJobReady(jobId)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Set job arrival
    job.setArrival(Timer::elapsedSeconds());

    // Find the job's canonical initial node
    int n = MyMpi::size(MPI_COMM_WORLD) - MyMpi::size(_comm);
    log(V4_VVER, "Creating permutation of size %i ...\n", n);
    AdjustablePermutation p(n, jobId);
    int nodeRank = p.get(0);

    JobRequest req(jobId, /*rootRank=*/-1, /*requestingNodeRank=*/_world_rank, 
        /*requestedNodeIndex=*/0, /*epoch=*/-1, /*numHops=*/0);
    req.timeOfBirth = job.getArrival();

    log(LOG_ADD_DESTRANK | V2_INFO, "Introducing job #%i", nodeRank, jobId);
    MyMpi::isend(MPI_COMM_WORLD, nodeRank, MSG_REQUEST_NODE, req);
    _introduced_job_ids.insert(jobId);
    _last_introduced_job_idx++;
}

void Client::checkClientDone() {
    
    bool jobQueueEmpty = _last_introduced_job_idx+1 >= (int)_ordered_job_ids.size();

    // If no jobs left and all introduced jobs done:
    if (!_file_adapter && jobQueueEmpty && _introduced_job_ids.empty()) {
        // All jobs are done
        log(V2_INFO, "All my jobs are terminated\n");
        int myRank = MyMpi::rank(MPI_COMM_WORLD);
        for (int i = MyMpi::size(MPI_COMM_WORLD)-MyMpi::size(_comm); i < MyMpi::size(MPI_COMM_WORLD); i++) {
            if (i != myRank) {
                MyMpi::isend(MPI_COMM_WORLD, i, MSG_CLIENT_FINISHED, IntVec({myRank}));
            }
        }
        _num_alive_clients--;
    }
}

void Client::handleRequestBecomeChild(MessageHandle& handle) {
    JobRequest req = Serializable::get<JobRequest>(handle.getRecvData());
    const JobDescription& desc = *_jobs[req.jobId];

    // Send job signature
    JobSignature sig(req.jobId, /*rootRank=*/handle.source, req.revision, desc.getTransferSize());
    MyMpi::isend(MPI_COMM_WORLD, handle.source, MSG_ACCEPT_ADOPTION_OFFER, sig);
    //stats.increment("sentMessages");
}

void Client::handleAckAcceptBecomeChild(MessageHandle& handle) {
    JobRequest req = Serializable::get<JobRequest>(handle.getRecvData());
    JobDescription& desc = *_jobs[req.jobId];
    assert(desc.getId() == req.jobId || log_return_false("%i != %i\n", desc.getId(), req.jobId));
    log(LOG_ADD_DESTRANK | V4_VVER, "Sending job desc. of #%i of size %i", handle.source, desc.getId(), desc.getTransferSize());
    _root_nodes[req.jobId] = handle.source;
    auto data = desc.getSerialization();

    int jobId = Serializable::get<int>(*data);    
    MyMpi::isend(MPI_COMM_WORLD, handle.source, MSG_SEND_JOB_DESCRIPTION, data);
    log(LOG_ADD_DESTRANK | V4_VVER, "Sent job desc. of #%i of size %i", handle.source, jobId, data->size());
}

void Client::handleJobDone(MessageHandle& handle) {
    IntPair recv = Serializable::get<IntPair>(handle.getRecvData());
    int jobId = recv.first;
    int resultSize = recv.second;
    log(LOG_ADD_SRCRANK | V4_VVER, "Will receive job result, length %i, for job #%i", handle.source, resultSize, jobId);
    MyMpi::isend(MPI_COMM_WORLD, handle.source, MSG_QUERY_JOB_RESULT, handle.getRecvData());
    MyMpi::irecv(MPI_COMM_WORLD, handle.source, MSG_SEND_JOB_RESULT, resultSize);
}

void Client::handleSendJobResult(MessageHandle& handle) {

    JobResult jobResult = Serializable::get<JobResult>(handle.getRecvData());
    int jobId = jobResult.id;
    int resultCode = jobResult.result;
    int revision = jobResult.revision;

    log(LOG_ADD_SRCRANK | V4_VVER, "Received result of job #%i rev. %i, code: %i", handle.source, jobId, revision, resultCode);
    JobDescription& desc = *_jobs[jobId];

    // Output response time and solution header
    log(V2_INFO, "RESPONSE_TIME #%i %.6f rev. %i\n", jobId, Timer::elapsedSeconds() - desc.getArrival(), revision);
    log(V2_INFO, "SOLUTION #%i %s rev. %i\n", jobId, resultCode == RESULT_SAT ? "SAT" : "UNSAT", revision);

    // Write full solution to file, if desired
    std::string baseFilename = _params.getParam("s2f");
    if (!baseFilename.empty()) {
        std::string filename = baseFilename + "_" + std::to_string(jobId);
        std::ofstream file;
        file.open(filename);
        if (!file.is_open()) {
            log(V0_CRIT, "ERROR: Could not open solution file\n");
        } else {
            file << "c SOLUTION #" << jobId << " rev. " << revision << " ";
            file << (resultCode == RESULT_SAT ? "SAT" : resultCode == RESULT_UNSAT ? "UNSAT" : "UNKNOWN") << "\n"; 
            for (auto lit : jobResult.solution) {
                if (lit == 0) continue;
                file << lit << " ";
            }
            file << "\n";
            file.close();
        }
    }

    if (_file_adapter) {
        _file_adapter->handleJobDone(jobResult);
    }

    if (_jobs[jobId]->isIncremental()) {
        if (desc.getRevision() > revision) {
            // Introduce next revision
            revision++;
            IntVec payload({jobId, revision});
            log(LOG_ADD_DESTRANK | V2_INFO, "Introducing #%i rev. %i", _root_nodes[jobId], jobId, revision);
            MyMpi::isend(MPI_COMM_WORLD, _root_nodes[jobId], MSG_NOTIFY_JOB_REVISION, payload);
        } else {
            // Job is completely done
            IntVec payload({jobId});
            MyMpi::isend(MPI_COMM_WORLD, _root_nodes[jobId], MSG_INCREMENTAL_JOB_FINISHED, payload);
            finishJob(jobId);
        }
    } else finishJob(jobId);
}

void Client::handleAbort(MessageHandle& handle) {

    IntVec request = Serializable::get<IntVec>(handle.getRecvData());
    int jobId = request[0];
    log(LOG_ADD_SRCRANK | V2_INFO, "TIMEOUT #%i %.6f", handle.source, jobId, Timer::elapsedSeconds() - _jobs[jobId]->getArrival());
    
    if (_file_adapter) {
        JobResult result;
        result.id = jobId;
        result.revision = _jobs[result.id]->getRevision();
        result.result = 0;
        _file_adapter->handleJobDone(result);
    }

    finishJob(jobId);
}

void Client::finishJob(int jobId) {

    // Clean up job
    _introduced_job_ids.erase(jobId);
    _jobs.erase(jobId);
    _root_nodes.erase(jobId);
    {
        auto lock = _job_ready_lock.getLock();
        _job_ready.erase(jobId);
    }

    // Report to other clients if all your jobs are done
    checkClientDone();

    // Employ "leaky bucket" as necessary
    int nextId = getNextIntroduceableJob();
    if (nextId >= 0) introduceJob(_jobs[nextId]);
}

void Client::handleQueryJobRevisionDetails(MessageHandle& handle) {

    IntVec request = Serializable::get<IntVec>(handle.getRecvData());
    int jobId = request[0];
    int firstRevision = request[1];
    int lastRevision = request[2];

    JobDescription& desc = *_jobs[jobId];
    IntVec response({jobId, firstRevision, lastRevision, desc.getTransferSize()});
    MyMpi::isend(MPI_COMM_WORLD, handle.source, MSG_SEND_JOB_REVISION_DETAILS, response);
}

void Client::handleAckJobRevisionDetails(MessageHandle& handle) {

    IntVec response = Serializable::get<IntVec>(handle.getRecvData());
    int jobId = response[0];
    int firstRevision = response[1];
    int lastRevision = response[2];
    //int transferSize = response[3];
    
    // TODO not implemented
    abort();
}

void Client::handleClientFinished(MessageHandle& handle) {
    // Some other client is done
    _num_alive_clients--;
}

void Client::handleExit(MessageHandle& handle) {
    Terminator::setTerminating();
}

void Client::readInstanceList(std::string& filename) {

    if (filename.empty()) return;

    log(V2_INFO, "Reading instances from file %s\n", filename.c_str());
    std::fstream file;
    file.open(filename, std::ios::in);
    if (!file.is_open()) {
        log(V0_CRIT, "ERROR: Could not open instance file - exiting\n");
        Logger::getMainInstance().flush();
        exit(1);
    }

    std::string line;
    bool jitterPriorities = _params.isNotNull("jjp");
    while (std::getline(file, line)) {
        if (line.substr(0, 1) == std::string("#")) {
            continue;
        }
        int id; float arrival; float priority; std::string instanceFilename;
        bool incremental;
        int pos = 0, next = line.find(" ");
        
        id = std::stoi(line.substr(pos, next-pos)); line = line.substr(next+1); next = line.find(" "); 
        arrival = std::stof(line.substr(pos, next-pos)); line = line.substr(next+1); next = line.find(" "); 
        priority = std::stof(line.substr(pos, next-pos)); line = line.substr(next+1); next = line.find(" "); 
        instanceFilename = line.substr(pos, next-pos); line = line.substr(next+1); next = line.find(" ");
        incremental = (line == "i");

        // Jitter job priority
        if (jitterPriorities) {
            priority *= 0.99 + 0.01 * Random::rand();
        }

        std::shared_ptr<JobDescription> job = std::make_shared<JobDescription>(id, priority, incremental);
        job->setArrival(arrival);
        _ordered_job_ids.push_back(id);
        _jobs[id] = job;
        _job_instances[id] = instanceFilename;
    }
    file.close();

    log(V2_INFO, "Read %i job instances from file %s\n", _jobs.size(), filename.c_str());
    //std::sort(jobs.begin(), jobs.end(), JobByArrivalComparator());
}

void Client::readFormula(std::string& filename, JobDescription& job) {

    SatReader r(filename);
    bool success = r.read(job);
    if (success) {
        log(V3_VERB, "Read %s\n", filename.c_str());
    } else {
        log(V1_WARN, "File %s could not be opened - skipping #%i\n", filename.c_str(), job.getId());
    }
}

Client::~Client() {
    if (_instance_reader_thread.joinable())
        _instance_reader_thread.join();

    _file_adapter.reset();

    // Merge logs from instance reader
    Logger::getMainInstance().mergeJobLogs(0);

    log(V4_VVER, "Leaving client destructor\n");
}