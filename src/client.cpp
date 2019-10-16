
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
    for (auto it = c.jobsByArrival.begin(); it != c.jobsByArrival.end(); ++it) {

        JobDescription &job = it->second;
        int jobId = job.getId();

        Console::log(Console::VERB, "Reading \"" + c.jobInstances[jobId] + "\" (#" + std::to_string(jobId) + ") ...");
        c.readFormula(c.jobInstances[jobId], job);
        if (job.getPayloadSize() > 1)
            Console::log(Console::VERB, "Read \"" + c.jobInstances[jobId] + "\" (#" + std::to_string(jobId) + ").");

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
    MyMpi::irecv(MPI_COMM_WORLD);
}

Client::~Client() {
    instanceReaderThread.join();
}

void Client::mainProgram() {

    auto it = jobsByArrival.begin();
    while (true) {

        // Introduce next job, if applicable
        if (it != jobsByArrival.end() && it->first <= Timer::elapsedSeconds()) {
        
            // Introduce job
            JobDescription& job = it->second;
            int jobId = job.getId();

            // Wait until job is ready to be sent
            while (true) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                std::unique_lock<std::mutex> lock(jobReadyLock);
                if (jobReady[jobId])
                    break;
            }
            it++;

            if (job.getPayloadSize() == 1) {
                // Some I/O error kept the instance from being read
                Console::log(Console::WARN, "Skipping job #" + std::to_string(jobId) + " due to previous I/O error");
                continue;
            }

            // Find the job's canonical initial node
            int n = MyMpi::size(MPI_COMM_WORLD) - MyMpi::size(comm);
            Console::log(Console::VERB, "Creating permutation of size " + std::to_string(n) + " ...");
            AdjustablePermutation p(n, jobId);
            int nodeRank = p.get(0);

            const JobSignature sig(jobId, nodeRank, job.getPayloadSize());

            Console::log_send(Console::INFO, "Introducing job #" + std::to_string(jobId), nodeRank);
            MyMpi::send(MPI_COMM_WORLD, nodeRank, MSG_INTRODUCE_JOB, sig); // TODO async?
            MyMpi::isend(MPI_COMM_WORLD, nodeRank, MSG_SEND_JOB, job);
        }

        // Poll messages, if present
        MessageHandlePtr handle;
        if ((handle = MyMpi::poll()) != NULL) {
            // Process message
            Console::log_recv(Console::VVERB, "Processing message of tag " + std::to_string(handle->tag), handle->source);

            if (handle->tag == MSG_JOB_DONE) {
                handleJobDone(handle);
            } else if (handle->tag == MSG_SEND_JOB_RESULT) {
                handleSendJobResult(handle);
            } else {
                Console::log_recv(Console::WARN, "Unknown message tag " + std::to_string(handle->tag) + "!", handle->source);
            }
        }

        // Listen to another message, if no listener is active
        if (!MyMpi::hasActiveHandles())
            MyMpi::irecv(MPI_COMM_WORLD);

        // Sleep for a bit
        usleep(1000); // 1000 = 1 millisecond
    }
}

void Client::handleJobDone(MessageHandlePtr handle) {

    int jobId = handle->recvData[0];
    int resultSize = handle->recvData[1];
    Console::log_recv(Console::VERB, "Preparing to receive job result of length " + std::to_string(resultSize) 
            + " for job #" + std::to_string(jobId), handle->source);
    MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_QUERY_JOB_RESULT, handle->recvData);
    MyMpi::irecv(MPI_COMM_WORLD, handle->source, MSG_SEND_JOB_RESULT, resultSize);
}

void Client::handleSendJobResult(MessageHandlePtr handle) {

    JobResult jobResult; jobResult.deserialize(handle->recvData);
    int jobId = jobResult.id;
    int resultCode = jobResult.result;

    Console::log_recv(Console::INFO, "Received result of job #" + std::to_string(jobId) 
                + ", code: " + std::to_string(resultCode), handle->source);
}

void Client::readInstanceList(std::string& filename) {

    Console::log(Console::INFO, "Reading instances from file " + filename);
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
        while (jobsByArrival.count(arrival)) {
            arrival += 0.00001f;
        }
        jobsByArrival[arrival] = job;
        jobInstances[id] = instanceFilename;
    }
    file.close();

    Console::log(Console::INFO, "Read " + std::to_string(jobsByArrival.size()) + " job instances from file " + filename);
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
        Console::log(Console::VERB, std::to_string(formula.size()) + " literals including separation zeros");

    } else {
        Console::log(Console::CRIT, "ERROR: File " + filename + " could not be opened. Skipping job #"
                   + std::to_string(job.getId()));
    }
}
