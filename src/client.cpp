
#include <iostream>
#include <fstream>
#include <string>
#include <chrono>
#include <thread>
#include <unistd.h>

#include "client.hpp"
#include "util/sat_reader.hpp"
#include "util/sys/timer.hpp"
#include "util/console.hpp"
#include "util/permutation.hpp"
#include "util/sys/proc.hpp"
#include "data/job_transfer.hpp"
#include "data/job_result.hpp"
#include "util/random.hpp"

// Executed by a separate worker thread
void Client::readAllInstances() {

    Console::log(Console::VERB, "FILE_IO started");

    for (size_t i = 0; i < _ordered_job_ids.size(); i++) {

        if (checkTerminate()) {
            Console::log(Console::VERB, "FILE_IO stopping");
            return;
        }

        // Keep at most 10 full jobs in memory at any time 
        while (i - _last_introduced_job_idx > 10) {
            if (checkTerminate()) {
                Console::log(Console::VERB, "FILE_IO stopping");
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        int jobId = _ordered_job_ids[i];
        JobDescription& job = *_jobs[jobId];
        if (job.isIncremental()) {
            Console::log(Console::VVERB, "FILE_IO Job #%i is incremental", jobId);
        }

        Console::log(Console::VERB, "FILE_IO reading \"%s\" (#%i)", _job_instances[jobId].c_str(), jobId);

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

        Console::log(Console::VERB, "FILE_IO read \"%s\" (#%i)", _job_instances[jobId].c_str(), jobId);

        auto lock = _job_ready_lock.getLock();
        _job_ready[jobId] = true;
    }
}

void Client::init() {

    _last_introduced_job_idx = -1;
    int internalRank = MyMpi::rank(_comm);
    std::string filename = _params.getFilename() + "." + std::to_string(internalRank);
    readInstanceList(filename);
    Console::log(Console::INFO, "Client main thread started");

    _instance_reader_thread = std::thread(&Client::readAllInstances, this);

    // Begin listening to incoming messages
    MyMpi::beginListening();

    Console::log(Console::VERB, "Global init barrier ...");
    MPI_Barrier(MPI_COMM_WORLD);
    Console::log(Console::VERB, "Passed global init barrier");
}

bool Client::checkTerminate() {

    if (Timer::globalTimelimReached(_params)) {
        Console::log(Console::INFO, "Global timeout: terminating");
        return true;
    }
    if (_num_alive_clients == 0) {
        // Send exit message to part of workers
        Console::log(Console::VERB, "Clients done: sending EXIT to workers");

        // Send MSG_EXIT to worker of rank 0, which will broadcast it
        MyMpi::isend(MPI_COMM_WORLD, 0, MSG_DO_EXIT, IntVec({0}));

        // Force sending all handles before exiting
        while (MyMpi::hasOpenSentHandles())
            MyMpi::testSentHandles();
        return true;
    }
    return false;
}

void Client::mainProgram() {

    float lastStatTime = Timer::elapsedSeconds();

    while (!checkTerminate()) {

        // Print memory usage info
        if (Timer::elapsedSeconds() - lastStatTime > 5) {
            double vm_usage, resident_set; int cpu;
            Proc::getSelfMemAndSchedCpu(cpu, vm_usage, resident_set);
            vm_usage *= 0.001 * 0.001;
            resident_set *= 0.001 * 0.001;
            Console::log(Console::VERB, "mem cpu=%i vm=%.4fGB rss=%.4fGB", cpu, vm_usage, resident_set);
            lastStatTime = Timer::elapsedSeconds();
        }

        // Introduce next job(s) as applicable
        // (only one job at a time to react better
        // to outside events without too much latency)
        int nextId = getNextIntroduceableJob();
        if (nextId >= 0) introduceJob(_jobs[nextId]);

        // Poll messages, if present
        std::vector<MessageHandlePtr> handles = MyMpi::poll();
        for (MessageHandlePtr& handle : handles) {
            // Process message
            Console::log_recv(Console::VVVERB, handle->source, "Processing msg, tag %i", handle->tag);

            if (handle->tag == MSG_NOTIFY_JOB_DONE) {
                handleJobDone(handle);
            } else if (handle->tag == MSG_SEND_JOB_RESULT) {
                handleSendJobResult(handle);
            } else if (handle->tag == MSG_NOTIFY_JOB_ABORTING) {
                handleAbort(handle);
            } else if (handle->tag == MSG_OFFER_ADOPTION) {
                handleRequestBecomeChild(handle);
            } else if (handle->tag == MSG_CONFIRM_ADOPTION) {
                handleAckAcceptBecomeChild(handle);
            } else if (handle->tag == MSG_QUERY_JOB_REVISION_DETAILS) {
                handleQueryJobRevisionDetails(handle);
            } else if (handle->tag == MSG_CONFIRM_JOB_REVISION_DETAILS) {
                handleAckJobRevisionDetails(handle);
            } else if (handle->tag == MSG_CLIENT_FINISHED) {
                handleClientFinished(handle);
            }  else if (handle->tag == MSG_DO_EXIT) {
                handleExit(handle);
            } else {
                Console::log_recv(Console::WARN, handle->source, "Unknown msg tag %i", handle->tag);
            }
        }

        MyMpi::testSentHandles();

        // Sleep for a bit
        usleep(1000); // 1000 = 1 millisecond
    }

    Console::flush();
    fflush(stdout);
}

int Client::getMaxNumParallelJobs() {
    std::string query = "lbc" + std::to_string(MyMpi::rank(_comm));
    return _params.getIntParam(query.c_str());
}

int Client::getNextIntroduceableJob() {
    
    // Are there any non-introduced jobs left?
    if (_last_introduced_job_idx+1 >= _ordered_job_ids.size()) return -1;
    
    // -- yes
    int jobId = _ordered_job_ids[_last_introduced_job_idx+1];
    bool introduce = true;
    
    // Check if there is space for another active job in this client's "bucket"
    int lbc = getMaxNumParallelJobs();
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

    if (job.getPayload(0) == nullptr || job.getPayload(0)->size() <= 1) {
        // Some I/O error kept the instance from being read
        Console::log(Console::WARN, "Skipping job #%i due to previous I/O error", jobId);
        return;
    }

    // Set job arrival
    job.setArrival(Timer::elapsedSeconds());

    // Find the job's canonical initial node
    int n = MyMpi::size(MPI_COMM_WORLD) - MyMpi::size(_comm);
    Console::log(Console::VVERB, "Creating permutation of size %i ...", n);
    AdjustablePermutation p(n, jobId);
    int nodeRank = p.get(0);

    const JobRequest req(jobId, /*rootRank=*/-1, /*requestingNodeRank=*/_world_rank, 
        /*requestedNodeIndex=*/0, /*epoch=*/-1, /*numHops=*/0);

    Console::log_send(Console::INFO, nodeRank, "Introducing job #%i", jobId);
    MyMpi::isend(MPI_COMM_WORLD, nodeRank, MSG_REQUEST_NODE, req);
    _introduced_job_ids.insert(jobId);
    _last_introduced_job_idx++;
}

void Client::checkClientDone() {
    
    bool jobQueueEmpty = _last_introduced_job_idx+1 >= _ordered_job_ids.size();

    // If no jobs left and all introduced jobs done:
    if (jobQueueEmpty && _introduced_job_ids.empty()) {
        // All jobs are done
        Console::log(Console::INFO, "All my jobs are terminated");
        int myRank = MyMpi::rank(MPI_COMM_WORLD);
        for (int i = MyMpi::size(MPI_COMM_WORLD)-MyMpi::size(_comm); i < MyMpi::size(MPI_COMM_WORLD); i++) {
            if (i != myRank) {
                MyMpi::isend(MPI_COMM_WORLD, i, MSG_CLIENT_FINISHED, IntVec({myRank}));
            }
        }
        _num_alive_clients--;
    }
}

void Client::handleRequestBecomeChild(MessageHandlePtr& handle) {
    JobRequest req = Serializable::get<JobRequest>(*handle->recvData);
    const JobDescription& desc = *_jobs[req.jobId];

    // Send job signature
    JobSignature sig(req.jobId, /*rootRank=*/handle->source, req.revision, desc.getTransferSize(false));
    MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_ACCEPT_ADOPTION_OFFER, sig);
    //stats.increment("sentMessages");
}

void Client::handleAckAcceptBecomeChild(MessageHandlePtr& handle) {
    JobRequest req = Serializable::get<JobRequest>(*handle->recvData);
    JobDescription& desc = *_jobs[req.jobId];
    assert(desc.getId() == req.jobId || Console::fail("%i != %i", desc.getId(), req.jobId));
    Console::log_send(Console::VERB, handle->source, "Sending job desc. of #%i of size %i", desc.getId(), desc.getTransferSize(false));
    _root_nodes[req.jobId] = handle->source;
    auto data = desc.serializeFirstRevision();

    int jobId = Serializable::get<int>(*data);    
    MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_SEND_JOB_DESCRIPTION, data);
    Console::log_send(Console::VERB, handle->source, "Sent job desc. of #%i of size %i", jobId, data->size());
}

void Client::handleJobDone(MessageHandlePtr& handle) {
    IntPair recv(*handle->recvData);
    int jobId = recv.first;
    int resultSize = recv.second;
    Console::log_recv(Console::VERB, handle->source, "Will receive job result, length %i, for job #%i", resultSize, jobId);
    MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_QUERY_JOB_RESULT, handle->recvData);
    MyMpi::irecv(MPI_COMM_WORLD, handle->source, MSG_SEND_JOB_RESULT, resultSize);
}

void Client::handleSendJobResult(MessageHandlePtr& handle) {

    JobResult jobResult = Serializable::get<JobResult>(*handle->recvData);
    int jobId = jobResult.id;
    int resultCode = jobResult.result;
    int revision = jobResult.revision;

    Console::log_recv(Console::INFO, handle->source, "Received result of job #%i rev. %i, code: %i", jobId, revision, resultCode);
    JobDescription& desc = *_jobs[jobId];

    // Output response time and solution header
    Console::log(Console::INFO, "RESPONSE_TIME #%i %.6f rev. %i", jobId, Timer::elapsedSeconds() - desc.getArrival(), revision);
    Console::log(Console::INFO, "SOLUTION #%i %s rev. %i", jobId, resultCode == 10 ? "SAT" : "UNSAT", revision);

    // Write full solution to file, if desired
    std::string baseFilename = _params.getParam("s2f");
    if (!baseFilename.empty()) {
        std::string filename = baseFilename + "_" + std::to_string(jobId);
        std::ofstream file;
        file.open(filename);
        if (!file.is_open()) {
            Console::log(Console::CRIT, "ERROR: Could not open solution file");
        } else {
            file << "c SOLUTION #" << jobId << " rev. " << revision << " ";
            file << (resultCode == 10 ? "SAT" : resultCode == 20 ? "UNSAT" : "UNKNOWN") << "\n"; 
            for (auto lit : jobResult.solution) {
                if (lit == 0) continue;
                file << lit << " ";
            }
            file << "\n";
            file.close();
        }
    }

    Console::logUnsafe(Console::VERB, ""); // line break
    Console::releaseLock();

    if (_jobs[jobId]->isIncremental() && desc.getRevision() > revision) {
        // Introduce next revision
        revision++;
        IntVec payload({jobId, revision});
        Console::log_send(Console::INFO, _root_nodes[jobId], "Introducing #%i rev. %i", jobId, revision);
        MyMpi::isend(MPI_COMM_WORLD, _root_nodes[jobId], MSG_NOTIFY_JOB_REVISION, payload);
    } else {
        // Job is completely done
        IntVec payload({jobId});
        MyMpi::isend(MPI_COMM_WORLD, _root_nodes[jobId], MSG_INCREMENTAL_JOB_FINISHED, payload);
        finishJob(jobId);
    }
}

void Client::handleAbort(MessageHandlePtr& handle) {

    IntVec request(*handle->recvData);
    int jobId = request[0];
    
    Console::log_recv(Console::INFO, handle->source, "TIMEOUT #%i %.6f", jobId, Timer::elapsedSeconds() - _jobs[jobId]->getArrival());
    finishJob(jobId);
}

void Client::finishJob(int jobId) {

    // Clean up job
    _introduced_job_ids.erase(jobId);
    _jobs.erase(jobId);

    // Report to other clients if all your jobs are done
    checkClientDone();

    // Employ "leaky bucket" as necessary
    int nextId = getNextIntroduceableJob();
    if (nextId >= 0) introduceJob(_jobs[nextId]);
}

void Client::handleQueryJobRevisionDetails(MessageHandlePtr& handle) {

    IntVec request(*handle->recvData);
    int jobId = request[0];
    int firstRevision = request[1];
    int lastRevision = request[2];

    JobDescription& desc = *_jobs[jobId];
    IntVec response({jobId, firstRevision, lastRevision, desc.getTransferSize(firstRevision, lastRevision)});
    MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_SEND_JOB_REVISION_DETAILS, response);
}

void Client::handleAckJobRevisionDetails(MessageHandlePtr& handle) {

    IntVec response(*handle->recvData);
    int jobId = response[0];
    int firstRevision = response[1];
    int lastRevision = response[2];
    //int transferSize = response[3];
    MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_SEND_JOB_REVISION_DATA, 
                _jobs[jobId]->serialize(firstRevision, lastRevision));
}

void Client::handleClientFinished(MessageHandlePtr& handle) {
    // Some other client is done
    _num_alive_clients--;
}

void Client::handleExit(MessageHandlePtr& handle) {
    Console::forceFlush();
    exit(1);
}

void Client::readInstanceList(std::string& filename) {

    Console::log(Console::INFO, "Reading instances from file %s", filename.c_str());
    std::fstream file;
    file.open(filename, std::ios::in);
    if (!file.is_open()) {
        Console::log(Console::CRIT, "ERROR: Could not open instance file - exiting");
        Console::forceFlush();
        exit(1);
    }

    std::string line;
    bool jitterPriorities = _params.isSet("jjp");
    while(std::getline(file, line)) {
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
        _jobs[id] = job;
        _ordered_job_ids.push_back(id);
        _job_instances[id] = instanceFilename;
    }
    file.close();

    Console::log(Console::INFO, "Read %i job instances from file %s", _jobs.size(), filename.c_str());
    //std::sort(jobs.begin(), jobs.end(), JobByArrivalComparator());
}

void Client::readFormula(std::string& filename, JobDescription& job) {

    SatReader r(filename);
    VecPtr formula = r.read();
    VecPtr assumptions = std::make_shared<std::vector<int>>();
    if (formula != NULL) {
        job.addPayload(formula);
        job.addAssumptions(assumptions);
        job.setNumVars(r.getNumVars());
        Console::log(Console::VERB, "%i literals including separation zeros, %i assumptions", formula->size(), assumptions->size());
    } else {
        Console::log(Console::WARN, "File %s could not be opened - skipping #%i", filename.c_str(), job.getId());
    }
}

Client::~Client() {
    if (_instance_reader_thread.joinable())
        _instance_reader_thread.join();

    Console::log(Console::VVERB, "Leaving client destructor");
}