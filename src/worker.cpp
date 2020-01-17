
#include <math.h>
#include <thread>
#include <unistd.h>
#include <iostream>
#include <fstream>
#include <initializer_list>

#include "worker.h"
#include "data/sat_job.h"
#include "util/timer.h"
#include "util/console.h"
#include "util/random.h"
#include "util/memusage.h"
#include "balancing/cutoff_priority_balancer.h"
#include "data/job_description.h"

void mpiMonitor(Worker* worker) {
    while (!worker->exiting) {
        double callStart = 0;
        std::string opName = MyMpi::currentCall(&callStart);
        if (callStart < 0.00001 || opName == "") {
            Console::log(Console::VERB, "MONITOR_MPI Not inside MPI call.");
        } else {
            double elapsed = Timer::elapsedSeconds() - callStart;
            Console::log(Console::VERB, "MONITOR_MPI Inside MPI call \"%s\" since %.4fs", opName.c_str(), elapsed);
        }
        usleep(1000 * 1000); // 1s
    }
}

void Worker::init() {

    // Initialize balancer
    //balancer = std::unique_ptr<Balancer>(new ThermodynamicBalancer(comm, params));
    balancer = std::unique_ptr<Balancer>(new CutoffPriorityBalancer(comm, params, stats));
    
    // Initialize pseudo-random order of nodes
    if (params.isSet("derandomize")) {
        // Pick fixed number k of bounce destinations
        int numBounceAlternatives = params.getIntParam("ba");
        int numWorkers = MyMpi::size(comm);
        assert(numBounceAlternatives % 2 == 0 || 
            Console::fail("ERROR: Parameter bounceAlternatives must be even (for theoretical reasons)."));
        assert(numBounceAlternatives < numWorkers || 
            Console::fail("ERROR: There must be more worker nodes than there are bounce alternatives per worker."));
        bounceAlternatives = std::vector<int>();

        // Generate global permutation over all worker ranks
        AdjustablePermutation p(numWorkers, /*random seed = */1);
        // Find the position where <worldRank> occurs in the permutation
        int i = 0;
        int x = p.get(i);
        while (x != worldRank) x = p.get(++i);
        Console::log(Console::VVERB, "My pos. in global permutation: %i", i);
        // Add neighbors in the permutation
        // to the node's bounce alternatives
        for (int j = i-(numBounceAlternatives/2); j < i; j++) {
            bounceAlternatives.push_back(p.get((j+numWorkers) % numWorkers));
        }
        for (int j = i+1; j <= i+(numBounceAlternatives/2); j++) {
            bounceAlternatives.push_back(p.get((j+numWorkers) % numWorkers));
        }
        assert(bounceAlternatives.size() == numBounceAlternatives);

        // Output found bounce alternatives
        std::string info = "";
        for (int i = 0; i < bounceAlternatives.size(); i++) {
            info += std::to_string(bounceAlternatives[i]) + " ";
        }
        Console::log(Console::VERB, "My bounce alternatives: %s", info.c_str());
    }

    // Send warm-up messages with your pseudorandom bounce destinations
    if (params.isSet("derandomize") && params.isSet("warmup")) {
        IntVec payload({1, 2, 3, 4, 5, 6, 7, 8});
        int numRuns = 5;
        for (int run = 0; run < numRuns; run++) {
            for (auto rank : bounceAlternatives) {
                MyMpi::isend(MPI_COMM_WORLD, rank, MSG_WARMUP, payload);
                Console::log_send(Console::VVERB, rank, "Warmup msg");
            }
        }
    }

    Console::log(Console::VERB, "Global initialization barrier ...");
    MPI_Barrier(MPI_COMM_WORLD);
    Console::log(Console::VERB, "Passed global initialization barrier.");

    // Begin listening to an incoming message
    MyMpi::beginListening(WORKER);

    mpiMonitorThread = std::thread(mpiMonitor, this);
}

bool Worker::checkTerminate() {
    if (exiting) return true;
    if (params.getFloatParam("T") > 0 && Timer::elapsedSeconds() > params.getFloatParam("T")) {
        Console::log(Console::INFO, "Global timeout: terminating.");
        return true;
    }
    return false;
}

void Worker::mainProgram() {

    int iteration = 0;
    float lastMemLogTime = Timer::elapsedSeconds();
    while (!checkTerminate()) {

        if (Timer::elapsedSeconds() - lastMemLogTime > 1.0) {
            // Print memory usage info
            double vm_usage, resident_set;
            process_mem_usage(vm_usage, resident_set);
            vm_usage *= 0.001 * 0.001;
            resident_set *= 0.001 * 0.001;
            Console::log(Console::VVERB, "mem vm=%.4fGB rss=%.4fGB", vm_usage, resident_set);
            lastMemLogTime = Timer::elapsedSeconds();
        }

        // If it is time to do balancing (and it is not being done right now)
        if (!balancer->isBalancing() && isTimeForRebalancing()) {

            // Rebalancing
            Console::log(MyMpi::rank(comm) == 0 ? Console::INFO : Console::VERB, 
                "Entering rebalancing of epoch %i", epochCounter.getEpoch());
            rebalance();

        } else if (balancer->isBalancing()) {

            // Advance balancing if possible (e.g. an iallreduce finished)
            if (balancer->canContinueBalancing()) {
                bool done = balancer->continueBalancing();
                if (done) finishBalancing();
            }
        }

        // Identify global stagnation of a rebalancing: Force abort
        if (epochCounter.getSecondsSinceLastSync() > params.getFloatParam("p") + 20.0) {
            // No rebalancing since t+20 seconds: Something is going wrong
            Console::log(Console::CRIT, "DESYNCHRONIZATION DETECTED -- Aborting.");
            Console::forceFlush();
            if (worldRank == 0) {
                for (int clientRank : clientNodes) {
                    Console::log_send(Console::INFO, clientRank, "Terminating client ...");
                    MyMpi::isend(MPI_COMM_WORLD, clientRank, MSG_EXIT, IntVec({0}));
                }
            }
            Console::forceFlush();
            exit(1);
        }

        MyMpi::testSentHandles();

        // Job communication (e.g. clause sharing)
        if (currentJob != NULL && currentJob->wantsToCommunicate()) {
            Console::log(Console::VERB, "%s wants to communicate", currentJob->toStr());
            currentJob->communicate();
        }

        MyMpi::testSentHandles();

        // Solve loop for active HordeLib instance
        float jobTime = 0;
        if (currentJob != NULL) {
            jobTime = Timer::elapsedSeconds();
            Job &job = *currentJob;

            bool initializing = job.isInitializing();
            int result = job.appl_solveLoop();

            if (result >= 0) {

                // Solver done!
                int jobRootRank = job.getRootNodeRank();
                IntVec payload({job.getId(), job.getRevision(), result});
                job.appl_dumpStats();

                // Signal termination to root -- may be a self message
                Console::log_send(Console::VERB, jobRootRank, "Sending finished info");
                MyMpi::isend(MPI_COMM_WORLD, jobRootRank, MSG_WORKER_FOUND_RESULT, payload);
                //stats.increment("sentMessages");

            } else if (!job.isRoot() && initializing && !job.isInitializing()) {
                // Non-root worker finished initialization
                IntVec payload({job.getId()});
                MyMpi::isend(MPI_COMM_WORLD, job.getParentNodeRank(), MSG_QUERY_VOLUME, payload);
            }

            jobTime = Timer::elapsedSeconds() - jobTime;
        }

        // Sleep for a bit
        //usleep(10); // 1000 = 1 millisecond

        // Poll a message, if present
        MessageHandlePtr handle;
        float pollTime = Timer::elapsedSeconds();
        if ((handle = MyMpi::poll()) != NULL) {
            pollTime = Timer::elapsedSeconds() - pollTime;
            Console::log(Console::VVVERB, "loop cycle %i", iteration);
            if (jobTime > 0) Console::log(Console::VVVERB, "job time: %.6f s", jobTime);
            Console::log(Console::VVVERB, "poll time: %.6f s", pollTime);

            // Process message
            Console::log_recv(Console::VVVERB, handle->source, "process msg, tag %i : %i", handle->tag, handle->recvData->at(0));
            float time = Timer::elapsedSeconds();

            //stats.increment("receivedMessages");

            if (handle->tag == MSG_FIND_NODE) {
                handleFindNode(handle);

            } else if (handle->tag == MSG_QUERY_VOLUME) {
                handleQueryVolume(handle);
            
            } else if (handle->tag == MSG_REQUEST_BECOME_CHILD)
                handleRequestBecomeChild(handle);

            else if (handle->tag == MSG_REJECT_BECOME_CHILD)
                handleRejectBecomeChild(handle);

            else if (handle->tag == MSG_ACCEPT_BECOME_CHILD)
                handleAcceptBecomeChild(handle);

            else if (handle->tag == MSG_ACK_ACCEPT_BECOME_CHILD)
                handleAckAcceptBecomeChild(handle);

            else if (handle->tag == MSG_SEND_JOB_DESCRIPTION)
                handleSendJob(handle);

            else if (handle->tag == MSG_UPDATE_VOLUME)
                handleUpdateVolume(handle);

            else if (handle->tag == MSG_JOB_COMMUNICATION)
                handleJobCommunication(handle);

            else if (handle->tag == MSG_WORKER_FOUND_RESULT)
                handleWorkerFoundResult(handle);

            else if (handle->tag == MSG_FORWARD_CLIENT_RANK)
                handleForwardClientRank(handle);
            
            else if (handle->tag == MSG_QUERY_JOB_RESULT)
                handleQueryJobResult(handle);

            else if (handle->tag == MSG_TERMINATE)
                handleTerminate(handle);

            else if (handle->tag == MSG_ABORT)
                handleAbort(handle);

            else if (handle->tag == MSG_WORKER_DEFECTING)
                handleWorkerDefecting(handle);

            else if (handle->tag == MSG_NOTIFY_JOB_REVISION)
                handleNotifyJobRevision(handle);
            
            else if (handle->tag == MSG_QUERY_JOB_REVISION_DETAILS)
                handleQueryJobRevisionDetails(handle);
            
            else if (handle->tag == MSG_SEND_JOB_REVISION_DETAILS)
                handleSendJobRevisionDetails(handle);
            
            else if (handle->tag == MSG_ACK_JOB_REVISION_DETAILS)
                handleAckJobRevisionDetails(handle);
            
            else if (handle->tag == MSG_SEND_JOB_REVISION_DATA)
                handleSendJobRevisionData(handle);

            else if (handle->tag == MSG_EXIT) 
                handleExit(handle);
            
            else if (handle->tag == MSG_COLLECTIVES) {
                // "Collectives" messages are currently handled only in balancer
                bool done = balancer->continueBalancing(handle);
                if (done) finishBalancing();

            } else if (handle->tag == MSG_WARMUP) {
                Console::log_recv(Console::VVVERB, handle->source, "Warmup msg");

            } else {
                Console::log_recv(Console::WARN, handle->source, "Unknown message tag %i", handle->tag);
            }

            MyMpi::testSentHandles();
            // Listen to message tag again as necessary
            MyMpi::resetListenerIfNecessary(WORKER, handle->tag);

            time = Timer::elapsedSeconds() - time;
            Console::log(Console::VVVERB, "processing msg, tag %i took %.4f s", handle->tag, time);
        }
        
        iteration++;
        usleep(10); // in microsecs
    }

    Console::flush();
    fflush(stdout);
}

void Worker::handleQueryVolume(MessageHandlePtr& handle) {

    IntVec payload(*handle->recvData);
    int jobId = payload[0];

    assert(hasJob(jobId));
    int volume = balancer->getVolume(jobId);
    IntVec response({jobId, volume});

    Console::log_send(Console::VERB, handle->source, "Responding to volume query for #%i with v=%i", jobId, volume);
    MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_UPDATE_VOLUME, response);
}

void Worker::handleFindNode(MessageHandlePtr& handle) {

    JobRequest req; req.deserialize(*handle->recvData);

    // Discard request if it originates from past epoch
    // (except if it is a request for a root node)
    if (req.epoch < epochCounter.getEpoch() && req.requestedNodeIndex > 0) {
        Console::log_recv(Console::INFO, handle->source, "Discarding job request %s from past epoch %i (I am in epoch %i)", 
                    jobStr(req.jobId, req.requestedNodeIndex).c_str(), req.epoch, epochCounter.getEpoch());
        return;
    }

    // Discard request if this job already finished to this node's knowledge
    if (hasJob(req.jobId) && jobs[req.jobId]->getState() == JobState::PAST) {
        Console::log(Console::INFO, "Discarding request %s as it already finished", 
                jobStr(req.jobId, req.requestedNodeIndex).c_str());
        return;
    }

    // Decide whether job should be adopted or bounced to another node
    bool adopts = false;
    int maxHops = maxJobHops(req.requestedNodeIndex == 0);
    if (isIdle() && !hasJobCommitments()) {
        // Node is idle and not committed to another job: OK
        adopts = true;

    } else if (req.numHops > maxHops && req.requestedNodeIndex > 0) {
        // Discard job request
        Console::log(Console::INFO, "Discarding job request %s which exceeded %i hops", 
                        jobStr(req.jobId, req.requestedNodeIndex).c_str(), maxHops);
        return;
    
    } else if (req.numHops > maxHops && req.requestedNodeIndex == 0 && !hasJobCommitments()) {
        // Request for a root node exceeded max #hops: Possibly adopt the job while dismissing the active job

        // If this node already computes on _this_ job, don't adopt it
        if (!hasJob(req.jobId) || getJob(req.jobId).isNotInState({ACTIVE, INITIALIZING_TO_ACTIVE})) {

            // Look for an active job that can be suspended
            if (currentJob != NULL) {
                Job& job = *currentJob;
                // Job must be active and a non-root leaf node
                if (job.isInState({ACTIVE, INITIALIZING_TO_ACTIVE}) 
                    && !job.isRoot() && !job.hasLeftChild() && !job.hasRightChild()) {
                    
                    // Inform parent node of the original job  
                    Console::log(Console::VERB, "Suspending %s ...", job.toStr());
                    Console::log(Console::VERB, "... in order to adopt starving job %s", 
                                    jobStr(req.jobId, req.requestedNodeIndex).c_str());  
                    IntPair pair(job.getId(), job.getIndex());
                    MyMpi::isend(MPI_COMM_WORLD, job.getParentNodeRank(), MSG_WORKER_DEFECTING, pair);
                    //stats.increment("sentMessages");

                    // Suspend this job
                    job.suspend();
                    setLoad(0, job.getId());

                    adopts = true;
                }
            }
        }
    } 
    
    if (adopts) {
        // Adoption takes place
        std::string jobstr = jobStr(req.jobId, req.requestedNodeIndex);
        Console::log_recv(Console::INFO, handle->source, "Willing to adopt %s after %i bounces", jobstr.c_str(), req.numHops);
        assert(isIdle() || Console::fail("Adopting a job, but not idle!"));
        //stats.push_back("bounces", req.numHops);

        // Commit on the job, send a request to the parent
        bool fullTransfer = false;
        if (!hasJob(req.jobId)) {
            // Job is not known yet: create instance, request full transfer
            jobs[req.jobId] = new SatJob(params, MyMpi::size(comm), worldRank, req.jobId, epochCounter);
            fullTransfer = true;
        } else if (!getJob(req.jobId).hasJobDescription()) {
            // Job is known, but never received full description; request full transfer
            fullTransfer = true;
        }
        req.fullTransfer = fullTransfer ? 1 : 0;
        jobs[req.jobId]->commit(req);
        jobCommitments[req.jobId] = req;
        MyMpi::isend(MPI_COMM_WORLD, req.requestingNodeRank, MSG_REQUEST_BECOME_CHILD, jobCommitments[req.jobId]);
        //stats.increment("sentMessages");

    } else {
        // Continue job finding procedure somewhere else
        bounceJobRequest(req, handle->source);
    }
}

void Worker::handleRequestBecomeChild(MessageHandlePtr& handle) {

    JobRequest req; req.deserialize(*handle->recvData);
    Console::log_recv(Console::VERB, handle->source, "Request to become parent of %s", 
                    jobStr(req.jobId, req.requestedNodeIndex).c_str());

    // Retrieve concerned job
    assert(hasJob(req.jobId));
    Job &job = getJob(req.jobId);

    // If request is for a root node, bump its epoch -- does not become obsolete
    if (req.epoch < epochCounter.getEpoch() && req.requestedNodeIndex == 0) {
        Console::log(Console::INFO, "Bumping epoch of root job request #%i:0", req.jobId);
        req.epoch = epochCounter.getEpoch();
    }

    // Check if node should be adopted or rejected
    bool reject = false;
    if (req.epoch < epochCounter.getEpoch()) {
        // Wrong (old) epoch
        Console::log_recv(Console::INFO, handle->source, "Discarding request %s from epoch %i (now is epoch %i)", 
                            job.toStr(), req.epoch, epochCounter.getEpoch());
        reject = true;

    } else if (job.isNotInState({ACTIVE, INITIALIZING_TO_ACTIVE})) {
        // Job is not active
        Console::log_recv(Console::INFO, handle->source, "%s is not active (any more) -- discarding", job.toStr());
        Console::log(Console::VERB, "Actual job state: %s", job.jobStateToStr());
        reject = true;
    
    } else if (req.requestedNodeIndex == job.getLeftChildIndex() && job.hasLeftChild()) {
        // Job already has a left child
        Console::log_recv(Console::INFO, handle->source, "Discarding request: %s already has a left child", job.toStr());
        reject = true;

    } else if (req.requestedNodeIndex == job.getRightChildIndex() && job.hasRightChild()) {
        // Job already has a right child
        Console::log_recv(Console::INFO, handle->source, "Discarding request: %s already has a right child", job.toStr());
        reject = true;

    } else {
        // Adopt the job
        // TODO Check if this node already has the full description!
        const JobDescription& desc = job.getDescription();

        // Send job signature
        JobSignature sig(req.jobId, req.rootRank, req.revision, desc.getTransferSize(true));
        MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_ACCEPT_BECOME_CHILD, sig);
        //stats.increment("sentMessages");

        // If req.fullTransfer, then wait for the child to acknowledge having received the signature
        if (req.fullTransfer == 1) {
            Console::log_send(Console::INFO, handle->source, "Sending %s", jobStr(req.jobId, req.requestedNodeIndex).c_str());
        } else {
            Console::log_send(Console::INFO, handle->source, "Resuming child %s", jobStr(req.jobId, req.requestedNodeIndex).c_str());
        }
        // Child *will* start / resume its job solvers
        // Mark new node as one of the node's children
        if (req.requestedNodeIndex == jobs[req.jobId]->getLeftChildIndex()) {
            jobs[req.jobId]->setLeftChild(handle->source);
        } else if (req.requestedNodeIndex == jobs[req.jobId]->getRightChildIndex()) {
            jobs[req.jobId]->setRightChild(handle->source);
        } else assert(req.requestedNodeIndex == 0);
    }

    // If rejected: Send message to rejected child node
    if (reject) {
        Console::log_send(Console::VERB, handle->source, "Rejecting potential child %s", jobStr(req.jobId, req.requestedNodeIndex).c_str());
        MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_REJECT_BECOME_CHILD, req);
        //stats.increment("sentMessages");
    }
}

void Worker::handleRejectBecomeChild(MessageHandlePtr& handle) {

    // Retrieve committed job
    JobRequest req; req.deserialize(*handle->recvData);
    assert(hasJob(req.jobId));
    Job &job = getJob(req.jobId);
    assert(job.isInState({COMMITTED, INITIALIZING_TO_COMMITTED})
            || Console::fail("Unexpected job state of %s : %s", job.toStr(), job.jobStateToStr()));

    // Erase commitment
    Console::log_recv(Console::INFO, handle->source, "Rejected to become %s : uncommitting", job.toStr());
    jobCommitments.erase(req.jobId);
    job.uncommit();
}

void Worker::handleAcceptBecomeChild(MessageHandlePtr& handle) {

    // Retrieve according job commitment
    JobSignature sig; sig.deserialize(*handle->recvData);
    assert(jobCommitments.count(sig.jobId));
    JobRequest& req = jobCommitments[sig.jobId];

    if (req.fullTransfer == 1) {
        // Full transfer of job description is required:
        // Send ACK to parent and receive full job description
        Console::log(Console::VERB, "Receiving job description of #%i of size %i", req.jobId, sig.getTransferSize());
        MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_ACK_ACCEPT_BECOME_CHILD, req);
        //stats.increment("sentMessages");
        MyMpi::irecv(MPI_COMM_WORLD, handle->source, MSG_SEND_JOB_DESCRIPTION, sig.getTransferSize()); // to be received later
        //stats.increment("receivedMessages");

    } else {
        // Already has job description: Directly resume job (if not terminated yet)
        assert(hasJob(req.jobId));
        Job& job = getJob(req.jobId);
        if (job.getState() != JobState::PAST) {
            Console::log_recv(Console::INFO, handle->source, "Starting or resuming %s (state: %s)", 
                        jobStr(req.jobId, req.requestedNodeIndex).c_str(), job.jobStateToStr());
            setLoad(1, req.jobId);
            job.reinitialize(req.requestedNodeIndex, req.rootRank, req.requestingNodeRank);
        }
        // Erase job commitment
        jobCommitments.erase(sig.jobId);
    }
}

void Worker::handleAckAcceptBecomeChild(MessageHandlePtr& handle) {
    JobRequest req; req.deserialize(*handle->recvData);

    // Retrieve and send concerned job description
    assert(hasJob(req.jobId));
    Job& job = getJob(req.jobId);
    // If job already terminated, the description contains the job id ONLY
    MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_SEND_JOB_DESCRIPTION, job.getSerializedDescription());
    //stats.increment("sentMessages");
    Console::log_send(Console::VERB, handle->source, "Sent full job description of %s", job.toStr());

    if (job.getState() == JobState::PAST) {
        // Job already terminated -- send termination signal
        IntPair pair(req.jobId, req.requestedNodeIndex);
        MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_TERMINATE, pair);
        //stats.increment("sentMessages");
    } else {

        // Mark new node as one of the node's children
        if (req.requestedNodeIndex == job.getLeftChildIndex()) {
            job.setLeftChild(handle->source);
        } else if (req.requestedNodeIndex == job.getRightChildIndex()) {
            job.setRightChild(handle->source);
        } else assert(req.requestedNodeIndex == 0);

        // Send current volume / initial demand update
        if (job.isInState({ACTIVE, INITIALIZING_TO_ACTIVE})) {
            int volume = balancer->getVolume(req.jobId);
            assert(volume >= 1);
            Console::log_send(Console::VERB, handle->source, "Propagating volume %i to new child", volume);
            IntPair jobIdAndVolume(req.jobId, volume);
            MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_UPDATE_VOLUME, jobIdAndVolume);
            //stats.increment("sentMessages");
        }
    }
}

void Worker::handleSendJob(MessageHandlePtr& handle) {
    auto data = handle->recvData;
    Console::log_recv(Console::VVVERB, handle->source, "Receiving job data of size %i", data->size());
    int jobId; memcpy(&jobId, data->data(), sizeof(int));
    assert(hasJob(jobId) || Console::fail("I don't know job #%i !", jobId));

    // Erase job commitment
    if (jobCommitments.count(jobId))
        jobCommitments.erase(jobId);

    if (handle->recvData->size() == sizeof(int)) {
        // Empty job description!
        Console::log(Console::VERB, "Received empty job description of #%i!", jobId);
        return;
    }

    // Initialize job inside a separate thread
    setLoad(1, jobId);
    getJob(jobId).beginInitialization();
    Console::log(Console::VERB, "Received full job description of #%i. Initializing ...", jobId);

    initializerThreads[jobId] = std::thread(&Worker::initJob, this, handle);
}

void Worker::initJob(MessageHandlePtr handle) {
    
    // Deserialize job description
    int jobId; memcpy(&jobId, handle->recvData->data(), sizeof(int));
    Console::log_recv(Console::VERB, handle->source, "Deserializing job #%i , description has size %i ...", jobId, handle->recvData->size());
    Job& job = getJob(jobId);
    job.setDescription(handle->recvData);

    // Remember arrival and initialize used CPU time (if root node)
    jobArrivals[jobId] = Timer::elapsedSeconds();
    if (job.isRoot()) {
        jobCpuTimeUsed[jobId] = 0;
    }
    
    if (job.isInState({INITIALIZING_TO_PAST})) {
        // Job was already aborted
        job.terminate();
    } else {
        // Initialize job
        job.appl_initialize();
    }
}

void Worker::handleUpdateVolume(MessageHandlePtr& handle) {
    IntPair recv(*handle->recvData);
    int jobId = recv.first;
    int volume = recv.second;
    if (!hasJob(jobId)) {
        Console::log(Console::WARN, "WARN: Received a volume update about #%i, which is unknown to me", jobId);
        return;
    }

    // Update volume assignment inside balancer and in actual job instance
    // (and its children)
    balancer->updateVolume(jobId, volume);
    updateVolume(jobId, volume);
}

void Worker::handleJobCommunication(MessageHandlePtr& handle) {

    // Deserialize job-specific message
    JobMessage msg; msg.deserialize(*handle->recvData);
    int jobId = msg.jobId;
    if (!hasJob(jobId)) {
        Console::log(Console::WARN, "Job message from unknown job #%i", jobId);
        return;
    }
    // Give message to corresponding job
    Job& job = getJob(jobId);
    job.appl_communicate(handle->source, msg);
}

void Worker::handleWorkerFoundResult(MessageHandlePtr& handle) {

    // Retrieve job
    IntVec res(*handle->recvData);
    int jobId = res[0];
    int revision = res[1];
    assert(hasJob(jobId) && getJob(jobId).isRoot());
    Console::log_recv(Console::VERB, handle->source, "Result has been found for job #%i", jobId);
    if (getJob(jobId).isInState({PAST, INITIALIZING_TO_PAST})) {
        Console::log_recv(Console::VERB, handle->source, "Discarding excess result for job #%i", jobId);
        return;
    }
    if (getJob(jobId).getRevision() > revision) {
        Console::log_recv(Console::VERB, handle->source, "Discarding obsolete result for job #%i rev. %i", jobId, revision);
        return;
    }

    // Redirect termination signal
    IntPair payload(jobId, getJob(jobId).getParentNodeRank());
    if (handle->source == worldRank) {
        // Self-message of root node: Directly send termination message to client
        informClient(payload.first, payload.second);
    } else {
        // Send rank of client node to the finished worker,
        // such that the worker can inform the client of the result
        MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_FORWARD_CLIENT_RANK, payload);
        //stats.increment("sentMessages");
        Console::log_send(Console::VERB, handle->source, "Sending client rank (%i)", payload.second); 
    }

    // Terminate job and propagate termination message
    if (getJob(jobId).getDescription().isIncremental()) {
        handleInterrupt(handle);
    } else {
        handleTerminate(handle);
    }
}

void Worker::handleForwardClientRank(MessageHandlePtr& handle) {

    // Receive rank of the job's client
    IntPair recv(*handle->recvData);
    int jobId = recv.first;
    int clientRank = recv.second;
    assert(hasJob(jobId));

    // Inform client of the found job result
    informClient(jobId, clientRank);
}

void Worker::handleQueryJobResult(MessageHandlePtr& handle) {

    // Receive acknowledgement that the client received the advertised result size
    // and wishes to receive the full job result
    int jobId; memcpy(&jobId, handle->recvData->data(), sizeof(int));
    assert(hasJob(jobId));
    const JobResult& result = getJob(jobId).getResult();
    Console::log_send(Console::VERB, handle->source, "Sending full job result to client");
    MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_SEND_JOB_RESULT, result);
    //stats.increment("sentMessages");
}

void Worker::handleTerminate(MessageHandlePtr& handle) {

    int jobId; memcpy(&jobId, handle->recvData->data(), sizeof(int));
    interruptJob(handle, jobId, /*terminate=*/true, /*reckless=*/false);
}

void Worker::handleInterrupt(MessageHandlePtr& handle) {

    int jobId; memcpy(&jobId, handle->recvData->data(), sizeof(int));
    interruptJob(handle, jobId, /*terminate=*/false, /*reckless=*/false);
}

void Worker::handleAbort(MessageHandlePtr& handle) {

    int jobId; memcpy(&jobId, handle->recvData->data(), sizeof(int));

    // Forward information on aborted job to client
    if (getJob(jobId).isRoot()) {
        MyMpi::isend(MPI_COMM_WORLD, getJob(jobId).getParentNodeRank(), MSG_ABORT, handle->recvData);
    }

    interruptJob(handle, jobId, /*terminate=*/true, /*reckless=*/true);
}

void Worker::handleWorkerDefecting(MessageHandlePtr& handle) {

    // Retrieve job
    IntPair recv(*handle->recvData);
    int jobId = recv.first;
    int index = recv.second;
    assert(hasJob(jobId));
    Job& job = getJob(jobId);

    // Which child is defecting?
    if (job.getLeftChildIndex() == index) {
        // Prune left child
        job.unsetLeftChild();
    } else if (job.getRightChildIndex() == index) {
        // Prune right child
        job.unsetRightChild();
    } else {
        Console::fail("%s : unknown child %s is defecting to another node", job.toStr(), jobStr(jobId, index).c_str());
    }
    
    int nextNodeRank;
    if (params.isSet("derandomize")) {
        nextNodeRank = Random::choice(bounceAlternatives);
    } else {
        nextNodeRank = getRandomWorkerNode();
    }

    // Initiate search for a replacement for the defected child
    Console::log(Console::VERB, "%s : trying to find a new child replacing defected node %s", 
                    job.toStr(), jobStr(jobId, index).c_str());
    JobRequest req(jobId, job.getRootNodeRank(), worldRank, index, epochCounter.getEpoch(), 0);
    MyMpi::isend(MPI_COMM_WORLD, nextNodeRank, MSG_FIND_NODE, req);
    //stats.increment("sentMessages");
}

void Worker::handleNotifyJobRevision(MessageHandlePtr& handle) {
    IntVec payload(*handle->recvData);
    int jobId = payload[0];
    int revision = payload[1];

    assert(hasJob(jobId));
    assert(getJob(jobId).isInState({STANDBY}) || Console::fail("%s : state %s", 
            getJob(jobId).toStr(), jobStateStrings[getJob(jobId).getState()]));

    // No other job running on this node
    assert(load == 1 && currentJob->getId() == jobId);

    // Request receiving information on revision size
    Job& job = getJob(jobId);
    int lastKnownRevision = job.getRevision();
    if (revision > lastKnownRevision) {
        Console::log(Console::VERB, "Received revision update #%i rev. %i (I am at rev. %i)", jobId, revision, lastKnownRevision);
        IntVec request({jobId, lastKnownRevision+1, revision});
        MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_QUERY_JOB_REVISION_DETAILS, request);
    } else {
        // TODO ???
        Console::log(Console::WARN, "Useless revision update #%i rev. %i (I am already at rev. %i)", jobId, revision, lastKnownRevision);
    }
}

void Worker::handleQueryJobRevisionDetails(MessageHandlePtr& handle) {
    IntVec request(*handle->recvData);
    int jobId = request[0];
    int firstRevision = request[1];
    int lastRevision = request[2];

    assert(hasJob(jobId));

    JobDescription& desc = getJob(jobId).getDescription();
    IntVec response({jobId, firstRevision, lastRevision, desc.getTransferSize(firstRevision, lastRevision)});
    MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_SEND_JOB_REVISION_DETAILS, response);
}

void Worker::handleSendJobRevisionDetails(MessageHandlePtr& handle) {
    IntVec response(*handle->recvData);
    //int jobId = response[0];
    int transferSize = response[3];
    MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_ACK_JOB_REVISION_DETAILS, handle->recvData);
    MyMpi::irecv(MPI_COMM_WORLD, handle->source, MSG_SEND_JOB_REVISION_DATA, transferSize);
}

void Worker::handleAckJobRevisionDetails(MessageHandlePtr& handle) {
    IntVec response(*handle->recvData);
    int jobId = response[0];
    int firstRevision = response[1];
    int lastRevision = response[2];
    //int transferSize = response[3];
    MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_SEND_JOB_REVISION_DATA, 
                getJob(jobId).getDescription().serialize(firstRevision, lastRevision));
}

void Worker::handleSendJobRevisionData(MessageHandlePtr& handle) {
    int jobId; memcpy(&jobId, handle->recvData->data(), sizeof(int));
    assert(hasJob(jobId) && getJob(jobId).isInState({STANDBY}));

    // TODO in separate thread
    Job& job = getJob(jobId);
    job.addAmendment(handle->recvData);
    int revision = job.getDescription().getRevision();
    Console::log(Console::INFO, "%s : computing on #%i rev. %i", job.toStr(), jobId, revision);
    
    // Propagate to children
    if (job.hasLeftChild()) {
        IntVec payload({jobId, revision});
        MyMpi::isend(MPI_COMM_WORLD, job.getLeftChildNodeRank(), MSG_NOTIFY_JOB_REVISION, payload);
    }
    if (job.hasRightChild()) {
        IntVec payload({jobId, revision});
        MyMpi::isend(MPI_COMM_WORLD, job.getRightChildNodeRank(), MSG_NOTIFY_JOB_REVISION, payload);
    }
}

void Worker::handleIncrementalJobFinished(MessageHandlePtr& handle) {
    int jobId; memcpy(&jobId, handle->recvData->data(), sizeof(int));
    interruptJob(handle, jobId, /*terminate=*/true, /*reckless=*/false);
}

void Worker::handleExit(MessageHandlePtr& handle) {
    Console::log_recv(Console::VERB, handle->source, "Received signal to exit.");
    exiting = true;
}

void Worker::interruptJob(MessageHandlePtr& handle, int jobId, bool terminate, bool reckless) {

    Job& job = getJob(jobId);

    // Do not terminate yet if the job is still in a committed state, because the job description should still arrive
    // (except if in reckless mode, where the description probably will never arrive from the parent)
    if (!reckless && job.isInState({COMMITTED, INITIALIZING_TO_COMMITTED})) {
        Console::log(Console::INFO, "Deferring interruption/termination handle, as job description did not arrive yet");
        MyMpi::deferHandle(handle); // do not consume this message while job state is "COMMITTED"
    }

    bool acceptMessage = job.isNotInState({NONE, STORED, PAST});
    if (acceptMessage) {

        // Propagate message down the job tree
        int msgTag;
        if (terminate && reckless) msgTag = MSG_ABORT;
        else if (terminate) msgTag = MSG_TERMINATE;
        else msgTag = MSG_INTERRUPT;
        if (job.hasLeftChild()) {
            MyMpi::isend(MPI_COMM_WORLD, job.getLeftChildNodeRank(), msgTag, handle->recvData);
            Console::log_send(Console::VERB, job.getLeftChildNodeRank(), "Propagating interruption of %s ...", job.toStr());
            //stats.increment("sentMessages");
        }
        if (job.hasRightChild()) {
            MyMpi::isend(MPI_COMM_WORLD, job.getRightChildNodeRank(), msgTag, handle->recvData);
            Console::log_send(Console::VERB, job.getRightChildNodeRank(), "Propagating interruption of %s ...", job.toStr());
            //stats.increment("sentMessages");
        }
        for (auto childRank : job.getPastChildren()) {
            MyMpi::isend(MPI_COMM_WORLD, childRank, msgTag, handle->recvData);
            Console::log_send(Console::VERB, childRank, "Propagating interruption of %s (past child) ...", job.toStr());
        }
        job.getPastChildren().clear();

        // Stop / terminate
        if (job.isInitializing() || job.isInState({ACTIVE, STANDBY, SUSPENDED})) {
            Console::log(Console::INFO, "Interrupting %s (state: %s)", job.toStr(), job.jobStateToStr());
            job.stop(); // Solvers are interrupted, not suspended!
            Console::log(Console::INFO, "%s : interrupted", job.toStr());
            if (terminate) {
                if (getLoad() && currentJob->getId() == job.getId()) setLoad(0, job.getId());
                job.terminate();
                Console::log(Console::INFO, "%s : terminated", job.toStr());
            } 
        }
    }
}

void Worker::informClient(int jobId, int clientRank) {
    const JobResult& result = getJob(jobId).getResult();

    // Send "Job done!" with advertised result size to client
    Console::log_send(Console::VERB, clientRank, "Sending JOB_DONE to client");
    IntPair payload(jobId, result.getTransferSize());
    MyMpi::isend(MPI_COMM_WORLD, clientRank, MSG_JOB_DONE, payload);
    //stats.increment("sentMessages");
}

void Worker::setLoad(int load, int whichJobId) {
    assert(load + this->load == 1); // (load WAS 1) XOR (load BECOMES 1)
    this->load = load;

    // Measure time for which worker was {idle,busy}
    float now = Timer::elapsedSeconds();
    //stats.add((load == 0 ? "busyTime" : "idleTime"), now - lastLoadChange);
    lastLoadChange = now;
    assert(hasJob(whichJobId));
    if (load == 1) {
        assert(currentJob == NULL);
        Console::log(Console::VERB, "LOAD 1 (+%s)", getJob(whichJobId).toStr());
        currentJob = &getJob(whichJobId);
    }
    if (load == 0) {
        assert(currentJob != NULL);
        Console::log(Console::VERB, "LOAD 0 (-%s)", getJob(whichJobId).toStr());
        currentJob = NULL;
    }
}

int Worker::getRandomWorkerNode() {

    // All clients are excluded from drawing
    std::set<int> excludedNodes = std::set<int>(this->clientNodes);
    // THIS node is also excluded from drawing
    excludedNodes.insert(worldRank);
    // Draw a node from the remaining nodes
    int randomOtherNodeRank = MyMpi::random_other_node(MPI_COMM_WORLD, excludedNodes);
    return randomOtherNodeRank;
}

void Worker::bounceJobRequest(JobRequest& request, int senderRank) {

    // Increment #hops
    request.numHops++;
    //stats.increment("bouncedJobs");
    int num = request.numHops;

    // Show warning if #hops is a large power of two
    if ((num >= 512) && ((num & (num - 1)) == 0)) {
        Console::log(Console::WARN, "%s bouncing for the %i. time", jobStr(request.jobId, request.requestedNodeIndex).c_str(), num);
    }

    int nextRank;
    if (params.isSet("derandomize")) {
        // Get random choice from bounce alternatives
        nextRank = Random::choice(bounceAlternatives);
        // while skipping the requesting node and the sender
        while (nextRank == request.requestingNodeRank || nextRank == senderRank) {
            nextRank = Random::choice(bounceAlternatives);
        }
    } else {
        // Generate pseudorandom permutation of this request
        int n = MyMpi::size(comm);
        AdjustablePermutation perm(n, 3 * request.jobId + 7 * request.requestedNodeIndex + 11 * request.requestingNodeRank);
        // Fetch next index of permutation based on number of hops
        int permIdx = request.numHops % n;
        nextRank = perm.get(permIdx);
        // (while skipping yourself, the requesting node, and the sender)
        while (nextRank == worldRank || nextRank == request.requestingNodeRank || nextRank == senderRank) {
            permIdx = (permIdx+1) % n;
            nextRank = perm.get(permIdx);
        }
    }

    // Send request to "next" worker node
    Console::log_send(Console::VVERB, nextRank, "Bouncing %s", jobStr(request.jobId, request.requestedNodeIndex).c_str());
    MyMpi::isend(MPI_COMM_WORLD, nextRank, MSG_FIND_NODE, request);
    //stats.increment("sentMessages");
}

void Worker::rebalance() {

    // Initiate balancing procedure
    bool done = balancer->beginBalancing(jobs);
    // If nothing to do, finish up balancing
    if (done) finishBalancing();
}

void Worker::finishBalancing() {
    // All collective operations are done; reset synchronized timer
    int lastSyncSeconds = epochCounter.getSecondsSinceLastSync();
    epochCounter.resetLastSync();

    // Retrieve balancing results
    Console::log(Console::VVVERB, "Finishing balancing ...");
    std::map<int, int> volumes = balancer->getBalancingResult();
    Console::log(MyMpi::rank(comm) == 0 ? Console::INFO : Console::VERB, "Rebalancing completed.");

    // Add last slice of idle/busy time 
    //stats.add((load == 1 ? "busyTime" : "idleTime"), Timer::elapsedSeconds() - lastLoadChange);
    lastLoadChange = Timer::elapsedSeconds();

    // Dump stats and advance to next epoch
    //stats.addResourceUsage();
    //stats.dump(epochCounter.getEpoch());
    if (currentJob != NULL) {
        currentJob->appl_dumpStats();

        if (currentJob->isRoot()) {
            int id = currentJob->getId();
            bool abort = false;
            
            // Calculate CPU seconds: (volume during last epoch) * #threads * (effective time of last epoch) 
            float newCpuTime = (jobVolumes.count(id) ? jobVolumes[id] : 1) * params.getIntParam("t") * lastSyncSeconds;
            jobCpuTimeUsed[id] += newCpuTime;
            float limit = params.getFloatParam("cpuh-per-instance")*3600.f;
            bool hasLimit = limit > 0;
            if (hasLimit) {
                Console::log(Console::INFO, "Job #%i spent %.3f/%.3f cpu seconds so far (%.3f in this epoch)", id, jobCpuTimeUsed[id], limit, newCpuTime);
            } else {
                Console::log(Console::INFO, "Job #%i spent %.3f cpu seconds so far (%.3f in this epoch)", id, jobCpuTimeUsed[id], newCpuTime);
            }
            if (hasLimit && jobCpuTimeUsed[id] > limit) {
                // Job exceeded its cpu time limit
                Console::log(Console::INFO, "Job #%i CPU TIMEOUT: aborting", id);
                abort = true;

            } else {

                // Calculate wall clock time
                float jobAge = currentJob->getAge();
                float timeLimit = params.getFloatParam("time-per-instance");
                if (timeLimit > 0 && jobAge > timeLimit) {
                    // Job exceeded its wall clock time limit
                    Console::log(Console::INFO, "Job #%i WALLCLOCK TIMEOUT: aborting", id);
                    abort = true;
                }
            }

            if (abort) {
                // "Virtual self message" aborting the job
                IntVec payload({id, currentJob->getRevision()});
                MessageHandlePtr handle(new MessageHandle(MyMpi::nextHandleId()));
                handle->source = worldRank;
                handle->recvData = payload.serialize();
                handle->tag = MSG_ABORT;
                handleAbort(handle);
            }
        }
    }
    epochCounter.increment();
    Console::log(Console::VERB, "Advancing to epoch %i", epochCounter.getEpoch());
    
    // Update volumes found during balancing, and trigger job expansions / shrinkings
    jobVolumes = volumes;
    for (auto it = volumes.begin(); it != volumes.end(); ++it) {
        Console::log(Console::INFO, "Job #%i : new volume %i", it->first, it->second);
        updateVolume(it->first, it->second);
    }
}

void Worker::updateVolume(int jobId, int volume) {

    Job &job = getJob(jobId);

    if (job.isNotInState({ACTIVE, INITIALIZING_TO_ACTIVE})) {
        // Job is not active right now
        return;
    }

    // Prepare volume update to propagate down the job tree
    IntPair payload(jobId, volume);

    // Root node update message
    int thisIndex = job.getIndex();
    if (thisIndex == 0) {
        Console::log(Console::VERB, "Updating volume of %s to %i", job.toStr(), volume);
    }

    // Left child
    int nextIndex = job.getLeftChildIndex();
    if (job.hasLeftChild()) {
        // Propagate left
        MyMpi::isend(MPI_COMM_WORLD, job.getLeftChildNodeRank(), MSG_UPDATE_VOLUME, payload);
        //stats.increment("sentMessages");
        if (nextIndex >= volume) {
            // Prune child
            Console::log_send(Console::VERB, job.getLeftChildNodeRank(), "Pruning left child of %s", job.toStr());
            job.unsetLeftChild();
        }
    } else if (job.hasJobDescription() && nextIndex < volume && !jobCommitments.count(jobId)) {
        // Grow left
        JobRequest req(jobId, job.getRootNodeRank(), worldRank, nextIndex, epochCounter.getEpoch(), 0);
        int nextNodeRank = job.getLeftChildNodeRank();
        MyMpi::isend(MPI_COMM_WORLD, nextNodeRank, MSG_FIND_NODE, req);
        //stats.increment("sentMessages");
    }

    // Right child
    nextIndex = job.getRightChildIndex();
    if (job.hasRightChild()) {
        // Propagate right
        MyMpi::isend(MPI_COMM_WORLD, job.getRightChildNodeRank(), MSG_UPDATE_VOLUME, payload);
        //stats.increment("sentMessages");
        if (nextIndex >= volume) {
            // Prune child
            Console::log_send(Console::VERB, job.getRightChildNodeRank(), "Pruning right child of %s", job.toStr());
            job.unsetRightChild();
        }
    } else if (job.hasJobDescription() && nextIndex < volume && !jobCommitments.count(jobId)) {
        // Grow right
        JobRequest req(jobId, job.getRootNodeRank(), worldRank, nextIndex, epochCounter.getEpoch(), 0);
        int nextNodeRank = job.getRightChildNodeRank();
        MyMpi::isend(MPI_COMM_WORLD, nextNodeRank, MSG_FIND_NODE, req);
        //stats.increment("sentMessages");
    }

    // Shrink (and pause solving) if necessary
    if (thisIndex > 0 && thisIndex >= volume) {
        jobs[jobId]->suspend();
        setLoad(0, jobId);
    }
}

bool Worker::isTimeForRebalancing() {
    return epochCounter.getSecondsSinceLastSync() >= params.getFloatParam("p");
}

Worker::~Worker() {

    exiting = true;

    for (auto idJobPair : jobs) {

        int id = idJobPair.first;
        Job* job = idJobPair.second;
        Console::log(Console::VVERB, "Cleaning up %s ...", job->toStr());

        // Join and delete initializer thread
        if (initializerThreads.count(id)) {
            Console::log(Console::VVERB, "Cleaning up init thread of %s ...", job->toStr());
            if (initializerThreads[id].joinable()) {
                initializerThreads[id].join();
            }
            initializerThreads.erase(id);
            Console::log(Console::VVERB, "Cleaned up init thread of %s.", job->toStr());
        }
        // Delete job and its solvers
        delete job;
    }

    if (mpiMonitorThread.joinable())
        mpiMonitorThread.join();

    Console::log(Console::VVERB, "Leaving destructor of worker environment.");
}