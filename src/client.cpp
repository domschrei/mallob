
#include <iostream>
#include <fstream>
#include <string>
#include <chrono>
#include <thread>
#include <unistd.h>

#include "client.h"
#include "util/sat_reader.h"
#include "util/timer.h"
#include "util/console.h"
#include "util/permutation.h"
#include "util/memusage.h"
#include "data/job_transfer.h"

void readAllInstances(Client* client) {

    Console::log(Console::VERB, "Started client I/O thread to read instances.");

    Client& c = *client;
    for (size_t i = 0; i < c.orderedJobIds.size(); i++) {

        if (c.checkTerminate()) {
            Console::log(Console::VERB, "Stopping instance reader thread");
            return;
        }

        // Keep at most 10 full jobs in memory at any time 
        while (i - c.lastIntroducedJobIdx > 10) {
            if (c.checkTerminate()) {
                Console::log(Console::VERB, "Stopping instance reader thread");
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        int jobId = c.orderedJobIds[i];
        JobDescription& job = *c.jobs[jobId];
        if (job.isIncremental()) {
            Console::log(Console::VVERB, "Incremental job #%i", jobId);
        }

        Console::log(Console::VERB, "Reading \"%s\" (#%i) ...", c.jobInstances[jobId].c_str(), jobId);

        if (job.isIncremental()) {
            int revision = 0;
            while (true) {
                std::fstream file;
                std::string filename = c.jobInstances[jobId] + "." + std::to_string(revision);
                file.open(filename, std::ios::in);
                if (file.is_open()) {
                    c.readFormula(filename, job);
                    job.setRevision(revision);
                } else {
                    break;
                }
                revision++;
            }
        } else {
            c.readFormula(c.jobInstances[jobId], job);
        }

        Console::log(Console::VERB, "Read \"%s\" (#%i).", c.jobInstances[jobId].c_str(), jobId);

        std::unique_lock<std::mutex> lock(c.jobReadyLock);
        c.jobReady[jobId] = true;
        lock.unlock();
    }
}

void Client::init() {

    lastIntroducedJobIdx = -1;
    int internalRank = MyMpi::rank(comm);
    std::string filename = params.getFilename() + "." + std::to_string(internalRank);
    readInstanceList(filename);
    Console::log(Console::INFO, "Started client main thread to introduce instances.");

    instanceReaderThread = std::thread(readAllInstances, this);

    // Begin listening to incoming messages
    MyMpi::beginListening(CLIENT);

    Console::log(Console::VERB, "Global initialization barrier ...");
    MPI_Barrier(MPI_COMM_WORLD);
    Console::log(Console::VERB, "Passed global initialization barrier.");
}

bool Client::checkTerminate() {
    if (params.getFloatParam("T") > 0 && Timer::elapsedSeconds() > params.getFloatParam("T")) {
        Console::log(Console::INFO, "Global timeout: terminating.");
        return true;
    }
    if (numAliveClients == 0) {
        // Send exit message to part of workers
        Console::log(Console::VERB, "Sending EXIT signal to workers.");
        int numClients = MyMpi::size(comm);
        int numWorkers = MyMpi::size(MPI_COMM_WORLD) - numClients;
        int workersPerClient = std::ceil(((float)numWorkers) / numClients);
        int left = MyMpi::rank(comm) * workersPerClient;
        int right = std::min(left + workersPerClient, numWorkers);
        assert(right <= numWorkers);
        for (int i = left; i < right; i++) {
            MyMpi::isend(MPI_COMM_WORLD, i, MSG_EXIT, IntVec({i}));
        }
        while (MyMpi::hasOpenSentHandles())
            MyMpi::testSentHandles();
        return true;
    }
    return false;
}

void Client::mainProgram() {

    float lastStatTime = Timer::elapsedSeconds();

    size_t i = 0;
    while (!checkTerminate()) {

        // Print memory usage info
        if (Timer::elapsedSeconds() - lastStatTime > 5) {
            double vm_usage, resident_set;
            process_mem_usage(vm_usage, resident_set);
            vm_usage *= 0.001 * 0.001;
            resident_set *= 0.001 * 0.001;
            Console::log(Console::VERB, "mem vm=%.4fGB rss=%.4fGB", vm_usage, resident_set);
            lastStatTime = Timer::elapsedSeconds();
        }

        // Introduce next job(s) as applicable:
        // Only one job at a time to react better
        // to outside events without too much latency!
        if (params.getIntParam("lbc") == 0) {
            if (i < orderedJobIds.size() && jobs[orderedJobIds[i]]->getArrival() <= Timer::elapsedSeconds()) {
                // Introduce job, if ready
                if (isJobReady(orderedJobIds[i])) {
                    introduceJob(jobs[orderedJobIds[i++]]);
                }
            }
        } else {
            if (params.getIntParam("lbc") > introducedJobIds.size() && lastIntroducedJobIdx+1 < orderedJobIds.size()) {
                // Introduce job, if ready
                int jobId = orderedJobIds[lastIntroducedJobIdx+1];
                if (isJobReady(jobId)) {
                    introduceJob(jobs[jobId]);
                }
            }
        }

        // Poll messages, if present
        MessageHandlePtr handle;
        if ((handle = MyMpi::poll()) != NULL) {
            // Process message
            Console::log_recv(Console::VVERB, handle->source, "Processing message of tag %i", handle->tag);

            if (handle->tag == MSG_JOB_DONE) {
                handleJobDone(handle);
            } else if (handle->tag == MSG_SEND_JOB_RESULT) {
                handleSendJobResult(handle);
            } else if (handle->tag == MSG_ABORT) {
                handleAbort(handle);
            } else if (handle->tag == MSG_REQUEST_BECOME_CHILD) {
                handleRequestBecomeChild(handle);
            } else if (handle->tag == MSG_ACK_ACCEPT_BECOME_CHILD) {
                handleAckAcceptBecomeChild(handle);
            } else if (handle->tag == MSG_QUERY_JOB_REVISION_DETAILS) {
                handleQueryJobRevisionDetails(handle);
            } else if (handle->tag == MSG_ACK_JOB_REVISION_DETAILS) {
                handleAckJobRevisionDetails(handle);
            } else if (handle->tag == MSG_CLIENT_FINISHED) {
                handleClientFinished(handle);
            }  else if (handle->tag == MSG_EXIT) {
                handleExit(handle);
            } else {
                Console::log_recv(Console::WARN, handle->source, "Unknown message tag %i", handle->tag);
            }

            // Re-listen to message tag as necessary
            MyMpi::resetListenerIfNecessary(CLIENT, handle->tag);
        }

        // Sleep for a bit
        //usleep(1000); // 1000 = 1 millisecond
    }

    Console::flush();
    fflush(stdout);
}

bool Client::isJobReady(int jobId) {
    std::unique_lock<std::mutex> lock(jobReadyLock);
    return jobReady.count(jobId) && jobReady[jobId];
}

void Client::introduceJob(std::shared_ptr<JobDescription>& jobPtr) {

    JobDescription& job = *jobPtr;
    int jobId = job.getId();

    // Wait until job is ready to be sent
    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        std::unique_lock<std::mutex> lock(jobReadyLock);
        if (jobReady.count(jobId) && jobReady[jobId])
            break;
    }

    if (job.getPayload(0)->size() <= 1) {
        // Some I/O error kept the instance from being read
        Console::log(Console::WARN, "Skipping job #%i due to previous I/O error", jobId);
        return;
    }

    // Set job arrival
    job.setArrival(Timer::elapsedSeconds());

    // Find the job's canonical initial node
    int n = MyMpi::size(MPI_COMM_WORLD) - MyMpi::size(comm);
    Console::log(Console::VERB, "Creating permutation of size %i ...", n);
    AdjustablePermutation p(n, jobId);
    int nodeRank = p.get(0);

    const JobRequest req(jobId, /*rootRank=*/-1, /*requestingNodeRank=*/worldRank, 
        /*requestedNodeIndex=*/0, /*epoch=*/-1, /*numHops=*/0);

    Console::log_send(Console::INFO, nodeRank, "Introducing job #%i", jobId);
    MyMpi::isend(MPI_COMM_WORLD, nodeRank, MSG_FIND_NODE, req);
    introducedJobIds.insert(jobId);
    lastIntroducedJobIdx++;
}

void Client::checkFinished() {
    
    bool jobQueueEmpty = lastIntroducedJobIdx+1 >= orderedJobIds.size();

    if (params.getIntParam("lbc") > 0 && jobQueueEmpty && introducedJobIds.empty()) {
        // All jobs are done
        Console::log(Console::INFO, "All of my jobs have been terminated.");
        int myRank = MyMpi::rank(MPI_COMM_WORLD);
        for (int i = MyMpi::size(MPI_COMM_WORLD)-MyMpi::size(comm); i < MyMpi::size(MPI_COMM_WORLD); i++) {
            if (i != myRank) {
                MyMpi::isend(MPI_COMM_WORLD, i, MSG_CLIENT_FINISHED, IntVec({myRank}));
            }
        }
        numAliveClients--;
    }
}

void Client::handleRequestBecomeChild(MessageHandlePtr& handle) {
    JobRequest req; req.deserialize(*handle->recvData);
    const JobDescription& desc = *jobs[req.jobId];

    // Send job signature
    JobSignature sig(req.jobId, /*rootRank=*/handle->source, req.revision, desc.getTransferSize(false));
    MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_ACCEPT_BECOME_CHILD, sig);
    //stats.increment("sentMessages");
}

void Client::handleAckAcceptBecomeChild(MessageHandlePtr& handle) {
    JobRequest req; req.deserialize(*handle->recvData);
    JobDescription& desc = *jobs[req.jobId];
    Console::log_send(Console::VERB, handle->source, "Sending job description of #%i of size %i", desc.getId(), desc.getTransferSize(false));
    rootNodes[req.jobId] = handle->source;
    MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_SEND_JOB_DESCRIPTION, desc.serializeFirstRevision());
    desc.clearPayload();
}

void Client::handleJobDone(MessageHandlePtr& handle) {
    IntPair recv(*handle->recvData);
    int jobId = recv.first;
    int resultSize = recv.second;
    Console::log_recv(Console::VERB, handle->source, "Preparing to receive job result of length %i for job #%i", resultSize, jobId);
    MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_QUERY_JOB_RESULT, handle->recvData);
    MyMpi::irecv(MPI_COMM_WORLD, handle->source, MSG_SEND_JOB_RESULT, resultSize);
}

void Client::handleSendJobResult(MessageHandlePtr& handle) {

    JobResult jobResult; jobResult.deserialize(*handle->recvData);
    int jobId = jobResult.id;
    int resultCode = jobResult.result;
    int revision = jobResult.revision;

    Console::log_recv(Console::INFO, handle->source, "Received result of job #%i rev. %i, code: %i", jobId, revision, resultCode);
    JobDescription& desc = *jobs[jobId];

    // Output response time and solution
    Console::log(Console::INFO, "RESPONSE_TIME #%i %.6f rev. %i", jobId, Timer::elapsedSeconds() - desc.getArrival(), revision);
    Console::getLock();
    Console::appendUnsafe(Console::VERB, "SOLUTION #%i rev. %i ", jobId, revision);
    bool head = false;
    for (auto it : jobResult.solution) {
        if (!head) {
            Console::appendUnsafe(Console::VERB, "%s ", it == 0 ? "SAT" : "UNSAT");
            head = true;
            if (it == 0) continue;
        }
        Console::appendUnsafe(Console::VERB, "%i ", it);
    }
    Console::logUnsafe(Console::VERB, "");
    Console::releaseLock();

    if (jobs[jobId]->isIncremental() && desc.getRevision() > revision) {
        // Introduce next revision
        revision++;
        IntVec payload({jobId, revision});
        Console::log_send(Console::INFO, rootNodes[jobId], "Introducing #%i rev. %i", jobId, revision);
        MyMpi::isend(MPI_COMM_WORLD, rootNodes[jobId], MSG_NOTIFY_JOB_REVISION, payload);
    } else {
        // Job is completely done
        IntVec payload({jobId});
        MyMpi::isend(MPI_COMM_WORLD, rootNodes[jobId], MSG_INCREMENTAL_JOB_FINISHED, payload);
        introducedJobIds.erase(jobId);
        jobs.erase(jobId);

        checkFinished();

        // Employ "leaky bucket"
        while (params.getIntParam("lbc") > introducedJobIds.size() && lastIntroducedJobIdx+1 < orderedJobIds.size()) {
            // Introduce a new job
            introduceJob(jobs[orderedJobIds[lastIntroducedJobIdx+1]]);
        }
    }
}

void Client::handleAbort(MessageHandlePtr& handle) {

    IntVec request(*handle->recvData);
    int jobId = request[0];
    
    Console::log_recv(Console::VERB, handle->source, "Acknowledging timeout of #%i.", jobId);
    Console::log(Console::INFO, "TIMEOUT #%i %.6f", jobId, Timer::elapsedSeconds() - jobs[jobId]->getArrival());
    introducedJobIds.erase(jobId);
    jobs.erase(jobId);

    checkFinished();

    // Employ "leaky bucket"
    while (params.getIntParam("lbc") > introducedJobIds.size() && lastIntroducedJobIdx+1 < orderedJobIds.size()) {
        // Introduce a new job
        introduceJob(jobs[orderedJobIds[lastIntroducedJobIdx+1]]);
    }
}

void Client::handleQueryJobRevisionDetails(MessageHandlePtr& handle) {

    IntVec request(*handle->recvData);
    int jobId = request[0];
    int firstRevision = request[1];
    int lastRevision = request[2];

    JobDescription& desc = *jobs[jobId];
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
                jobs[jobId]->serialize(firstRevision, lastRevision));
}

void Client::handleClientFinished(MessageHandlePtr& handle) {
    numAliveClients--;
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
        Console::log(Console::CRIT, "ERROR: Could not open instance file! Exiting.");
        Console::forceFlush();
        exit(1);
    }

    std::string line;
    while(std::getline(file, line)) {
        if (line.substr(0, 1) == std::string("#")) {
            continue;
        }
        int id; float arrival; float priority; std::string instanceFilename; bool incremental;
        int pos = 0, next = 0;
        next = line.find(" "); id = std::stoi(line.substr(pos, next-pos)); line = line.substr(next+1);
        next = line.find(" "); arrival = std::stof(line.substr(pos, next-pos)); line = line.substr(next+1);
        next = line.find(" "); priority = std::stof(line.substr(pos, next-pos)); line = line.substr(next+1);
        next = line.find(" "); instanceFilename = line.substr(pos, next-pos); line = line.substr(next+1);
        incremental = (line == "i");
        std::shared_ptr<JobDescription> job = std::make_shared<JobDescription>(id, priority, incremental);
        job->setArrival(arrival);
        jobs[id] = job;
        orderedJobIds.push_back(id);
        jobInstances[id] = instanceFilename;
    }
    file.close();

    Console::log(Console::INFO, "Read %i job instances from file %s", jobs.size(), filename.c_str());
    //std::sort(jobs.begin(), jobs.end(), JobByArrivalComparator());
}

void Client::readFormula(std::string& filename, JobDescription& job) {

    /*
    std::fstream file;
    file.open(filename, std::ios::in);
    if (file.is_open()) {

        VecPtr formula = std::make_shared<std::vector<int>>();
        VecPtr assumptions = std::make_shared<std::vector<int>>();

        std::string line;
        while(std::getline(file, line)) {
            int pos = 0;
            int next = 0; 
            while (pos < line.length() && line[pos] == ' ') pos++;
            if (pos >= line.length() || line[pos] == 'c' 
                    || line[pos] == 'p') {
                continue;
            }
            while (true) {

                // Find end position of next symbol
                next = pos;
                bool nonwhitespace = false;
                while (next < line.length()) {
                    if (nonwhitespace && line[next] == ' ')
                        break;
                    if (line[next] != ' ') nonwhitespace = true;
                    next++;
                }

                if (next >= line.length()) {
                    // clause ended
                    formula->push_back(0);
                    break;
                } else {
                    int lit; int asmpt = 0;
                    assert(line[next] == ' ');
                    if (line[next-1] == '!') {
                        asmpt = std::stoi(line.substr(pos, next-pos-1));
                        assumptions->push_back(asmpt);
                        break;
                    } else {
                        lit = std::stoi(line.substr(pos, next-pos));
                        if (lit != 0) formula->push_back(lit);
                        pos = next+1;
                    }
                }
            }
        }
        job.addPayload(formula);
        job.addAssumptions(assumptions);

        Console::log(Console::VERB, "%i literals including separation zeros, %i assumptions", formula->size(), assumptions->size());

    } else {
        Console::log(Console::CRIT, "ERROR: File %s could not be opened. Skipping job #%i", filename.c_str(), job.getId());
    }
    */

    SatReader r(filename);
    VecPtr formula = r.read();
    VecPtr assumptions = std::make_shared<std::vector<int>>();
    if (formula != NULL) {
        job.addPayload(formula);
        job.addAssumptions(assumptions);
        Console::log(Console::VERB, "%i literals including separation zeros, %i assumptions", formula->size(), assumptions->size());
    } else {
        Console::log(Console::CRIT, "ERROR: File %s could not be opened. Skipping job #%i", filename.c_str(), job.getId());
    }
}

Client::~Client() {
    if (instanceReaderThread.joinable())
        instanceReaderThread.join();

    Console::log(Console::VVERB, "Leaving destructor of client environment.");
}