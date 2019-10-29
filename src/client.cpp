
#include <iostream>
#include <fstream>
#include <string>
#include <chrono>
#include <thread>
#include <unistd.h>

#include "client.h"
#include "util/timer.h"
#include "util/console.h"
#include "util/permutation.h"
#include "data/job_transfer.h"

void readAllInstances(Client* client) {

    Console::log(Console::VERB, "Started client I/O thread to read instances.");

    Client& c = *client;
    for (size_t i = 0; i < c.jobs.size(); i++) {

        JobDescription &job = c.jobs[i];
        int jobId = job.getId();

        Console::log(Console::VERB, "Reading \"%s\" (#%i) ...", c.jobInstances[jobId].c_str(), jobId);
        c.readFormula(c.jobInstances[jobId], job);
        if (job.getPayload().size() > 1)
            Console::log(Console::VERB, "Read \"%s\" (#%i).", c.jobInstances[jobId].c_str(), jobId);

        std::unique_lock<std::mutex> lock(c.jobReadyLock);
        c.jobReady[jobId] = true;
        lock.unlock();
    }
}

void Client::init() {

    int internalRank = MyMpi::rank(comm);
    std::string filename = params.getFilename() + "." + std::to_string(internalRank);
    readInstanceList(filename);
    Console::log(Console::INFO, "Started client main thread to introduce instances.");

    instanceReaderThread = std::thread(readAllInstances, this);

    // Begin listening to incoming messages
    MyMpi::listen();
}

Client::~Client() {
    instanceReaderThread.join();
}

void Client::mainProgram() {

    size_t i = 0;
    while (true) {

        // Introduce next job(s), if applicable
        while (i < jobs.size() && jobs[i].getArrival() <= Timer::elapsedSeconds()) {
        
            // Introduce job
            JobDescription& job = jobs[i++];
            int jobId = job.getId();

            // Wait until job is ready to be sent
            while (true) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                std::unique_lock<std::mutex> lock(jobReadyLock);
                if (jobReady[jobId])
                    break;
            }

            if (job.getPayload().size() <= 1) {
                // Some I/O error kept the instance from being read
                Console::log(Console::WARN, "Skipping job #%i due to previous I/O error", jobId);
                continue;
            }

            // Find the job's canonical initial node
            int n = MyMpi::size(MPI_COMM_WORLD) - MyMpi::size(comm);
            Console::log(Console::VERB, "Creating permutation of size %i ...", n);
            AdjustablePermutation p(n, jobId);
            int nodeRank = p.get(0);

            const JobRequest req(jobId, /*rootRank=*/-1, /*requestingNodeRank=*/worldRank, 
                /*requestedNodeIndex=*/0, /*epoch=*/-1, /*numHops=*/0);

            Console::log_send(Console::INFO, nodeRank, "Introducing job #%i", jobId);
            MyMpi::isend(MPI_COMM_WORLD, nodeRank, MSG_FIND_NODE, req);
            introducedJobs[jobId] = &job;
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
            } else if (handle->tag == MSG_REQUEST_BECOME_CHILD) {
                handleRequestBecomeChild(handle);
            } else if (handle->tag == MSG_ACK_ACCEPT_BECOME_CHILD) {
                handleAckAcceptBecomeChild(handle);
            } else {
                Console::log_recv(Console::WARN, handle->source, "Unknown message tag %i", handle->tag);
            }
        }

        // Listen to another message, if no listener is active
        MyMpi::listen();

        // Sleep for a bit
        usleep(1000); // 1000 = 1 millisecond
    }
}

void Client::handleRequestBecomeChild(MessageHandlePtr handle) {
    JobRequest req; req.deserialize(handle->recvData);
    const JobDescription& desc = *introducedJobs[req.jobId];

    // Send job signature
    JobSignature sig(req.jobId, /*rootRank=*/handle->source, desc.getTransferSize());
    MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_ACCEPT_BECOME_CHILD, sig);
    stats.increment("sentMessages");
}

void Client::handleAckAcceptBecomeChild(MessageHandlePtr handle) {
    JobRequest req; req.deserialize(handle->recvData);
    const JobDescription& desc = *introducedJobs[req.jobId];
    Console::log_send(Console::VERB, handle->source, "Sending job description of #%i of size %i", desc.getId(), desc.getTransferSize());
    MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_SEND_JOB, desc);
}

void Client::handleJobDone(MessageHandlePtr handle) {
    IntPair recv(handle->recvData);
    int jobId = recv.first;
    int resultSize = recv.second;
    Console::log_recv(Console::VERB, handle->source, "Preparing to receive job result of length %i for job #%i", resultSize, jobId);
    MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_QUERY_JOB_RESULT, handle->recvData);
    MyMpi::irecv(MPI_COMM_WORLD, handle->source, MSG_SEND_JOB_RESULT, resultSize);
}

void Client::handleSendJobResult(MessageHandlePtr handle) {

    JobResult jobResult; jobResult.deserialize(handle->recvData);
    int jobId = jobResult.id;
    int resultCode = jobResult.result;

    Console::log_recv(Console::INFO, handle->source, "Received result of job #%i, code: %i", jobId, resultCode);

    // Output response time and solution
    Console::log(Console::INFO, "RESPONSE_TIME #%i %.6f", jobId, Timer::elapsedSeconds() - introducedJobs[jobId]->getArrival());
    Console::getLock();
    Console::appendUnsafe(Console::VERB, "SOLUTION #%i ", jobId);
    for (auto it : jobResult.solution) {
        Console::appendUnsafe(Console::VERB, "%i ", it);
    }
    Console::logUnsafe(Console::VERB, "");
    Console::releaseLock();
}

void Client::readInstanceList(std::string& filename) {

    Console::log(Console::INFO, "Reading instances from file %s", filename.c_str());
    std::fstream file;
    file.open(filename, std::ios::in);
    if (!file.is_open()) {
        Console::log(Console::CRIT, "ERROR: Could not open instance file! Will stay idle.");
        return;
    }

    std::string line;
    while(std::getline(file, line)) {
        if (line.substr(0, 1) == std::string("#")) {
            continue;
        }
        int id; float arrival; float priority; std::string instanceFilename;
        int pos = 0, next = 0;
        next = line.find(" "); id = std::stoi(line.substr(pos, next-pos)); line = line.substr(next+1);
        next = line.find(" "); arrival = std::stof(line.substr(pos, next-pos)); line = line.substr(next+1);
        next = line.find(" "); priority = std::stof(line.substr(pos, next-pos)); line = line.substr(next+1);
        next = line.find(" "); instanceFilename = line.substr(pos, next-pos); line = line.substr(next+1);
        JobDescription job(id, priority);
        job.setArrival(arrival);
        jobs.push_back(job);
        jobInstances[id] = instanceFilename;
    }
    file.close();

    Console::log(Console::INFO, "Read %i job instances from file %s", jobs.size(), filename.c_str());
    std::sort(jobs.begin(), jobs.end(), JobByArrivalComparator());
}

void Client::readFormula(std::string& filename, JobDescription& job) {

    std::fstream file;
    file.open(filename, std::ios::in);
    if (file.is_open()) {

        std::vector<int> formula;
        std::string line;
        while(std::getline(file, line)) {
            if (line.substr(0, 1) == std::string("c") || line.substr(0, 1) == std::string("p")) {
                continue;
            }
            int next = 0; int pos = 0;
            while (true) {
                next = line.find(" ", pos);
                int lit;
                if (next < 0)
                    lit = 0;
                else
                    lit = std::stoi(line.substr(pos, pos+next));
                formula.push_back(lit);
                if (next < 0)
                    break;
                pos = next+1;
            }
        }
        job.setPayload(formula);
        Console::log(Console::VERB, "%i literals including separation zeros", formula.size());

    } else {
        Console::log(Console::CRIT, "ERROR: File %s could not be opened. Skipping job #%i", filename.c_str(), job.getId());
    }
}
