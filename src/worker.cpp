
#include <math.h>
#include <thread>
#include <unistd.h>
#include <iostream>
#include <fstream>

#include "worker.h"
#include "data/sat_job.h"
#include "util/timer.h"
#include "util/console.h"
#include "util/random.h"
#include "balancing/thermodynamic_balancer.h"
#include "balancing/cutoff_priority_balancer.h"

void Worker::init() {

    // Initialize balancer
    //balancer = std::unique_ptr<Balancer>(new ThermodynamicBalancer(comm, params));
    balancer = std::unique_ptr<Balancer>(new CutoffPriorityBalancer(comm, params, stats));
    
    // Begin listening to an incoming message
    MyMpi::beginListening(WORKER);

    Console::log(Console::VERB, "Global initialization barrier ...");
    MPI_Barrier(MPI_COMM_WORLD);
    Console::log(Console::VERB, "Passed global initialization barrier.");
}

void Worker::checkTerminate() {

    std::fstream file;
    file.open("TERMINATE_GLOBALLY_NOW", std::ios::in);
    if (file.is_open()) {
        file.close();
        Console::log(Console::WARN, "Acknowledged TERMINATE_GLOBALLY_NOW. Exiting.");
        exit(0);
    }
}

void Worker::mainProgram() {

    while (true) {

        // If it is time to do balancing (and it is not being done right now)
        if (!balancer->isBalancing() && isTimeForRebalancing()) {
            
            // Check if termination signal file exists
            checkTerminate();

            // Rebalancing
            Console::log(MyMpi::rank(comm) == 0 ? Console::INFO : Console::VERB, 
                "Entering rebalancing of epoch %i", epochCounter.getEpoch());
            rebalance();

        } else if (balancer->isBalancing()) {

            // Advance balancing if possible (e.g. an ireduce finished)
            if (balancer->canContinueBalancing()) {
                bool done = balancer->continueBalancing();
                if (done) finishBalancing();
            }
        }

        // Job communication (e.g. clause sharing)
        for (auto it : jobs) {
            Job& job = *it.second;
            if (job.wantsToCommunicate()) {
                Console::log(Console::VERB, "%s wants to communicate", job.toStr());
                job.communicate();
            }
        }

        // Solve loops for each active HordeLib instance
        for (auto it : jobs) {
            int jobId = it.first;
            Job &job = *it.second;

            int result = job.solveLoop();
            if (result >= 0) {

                // Solver done!
                int jobRootRank = job.getRootNodeRank();
                IntPair pair(jobId, (int) result);
                job.dumpStats();

                // Signal termination to root -- may be a self message
                Console::log_send(Console::VERB, jobRootRank, "Sending finished info");
                MyMpi::isend(MPI_COMM_WORLD, jobRootRank, MSG_WORKER_FOUND_RESULT, pair);
                stats.increment("sentMessages");
            }
        }

        // Sleep for a bit
        usleep(10); // 1000 = 1 millisecond

        // Poll messages, if present
        MessageHandlePtr handle;
        if ((handle = MyMpi::poll()) != NULL) {
            // Process message
            Console::log_recv(Console::VVVERB, handle->source, "Processing message of tag %i", handle->tag);
            stats.increment("receivedMessages");

            if (handle->tag == MSG_FIND_NODE)
                handleFindNode(handle);

            else if (handle->tag == MSG_REQUEST_BECOME_CHILD)
                handleRequestBecomeChild(handle);

            else if (handle->tag == MSG_REJECT_BECOME_CHILD)
                handleRejectBecomeChild(handle);

            else if (handle->tag == MSG_ACCEPT_BECOME_CHILD)
                handleAcceptBecomeChild(handle);

            else if (handle->tag == MSG_ACK_ACCEPT_BECOME_CHILD)
                handleAckAcceptBecomeChild(handle);

            else if (handle->tag == MSG_SEND_JOB)
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

            else if (handle->tag == MSG_WORKER_DEFECTING)
                handleWorkerDefecting(handle);
            
            else if (handle->tag == MSG_COLLECTIVES) {
                // "Collectives" messages are currently handled only in balancer
                bool done = balancer->handleMessage(handle);
                if (done) finishBalancing();

            } else {
                Console::log_recv(Console::WARN, handle->source, "Unknown message tag %i", handle->tag);
            }

            // Listen to message tag again as necessary
            MyMpi::resetListenerIfNecessary(WORKER, handle->tag);
        }
    }
}

void Worker::handleFindNode(MessageHandlePtr& handle) {

    JobRequest req; req.deserialize(*handle->recvData);

    // Discard request if it originates from past epoch
    // (except if it is a request for a root node)
    if (req.epoch < epochCounter.getEpoch() && req.requestedNodeIndex > 0) {
        Console::log_recv(Console::INFO, handle->source, "Discarding job request %s from past epoch %i (I am in epoch %i)", 
                    jobStr(req.jobId, req.requestedNodeIndex), req.epoch, epochCounter.getEpoch());
        return;
    }

    // Discard request if this job already finished to this node's knowledge
    if (hasJob(req.jobId) && jobs[req.jobId]->getState() == JobState::PAST) {
        Console::log(Console::INFO, "Discarding request %s as it already finished", 
                jobStr(req.jobId, req.requestedNodeIndex));
        return;
    }

    // Decide whether job should be adopted or bounced to another node
    bool adopts = false;
    if (isIdle() && !hasJobCommitments()) {
        // Node is idle and not committed to another job: OK
        adopts = true;

    } else if (req.numHops > maxJobHops() && req.requestedNodeIndex > 0) {
        // Discard job request
        Console::log(Console::INFO, "Discarding job request %s which exceeded %i hops", 
                        jobStr(req.jobId, req.requestedNodeIndex), maxJobHops());
        return;
    
    } else if (req.numHops > maxJobHops() && req.requestedNodeIndex == 0 && !hasJobCommitments()) {
        // Request for a root node exceeded max #hops: Possibly adopt the job while dismissing the active job

        // If this node already computes on _this_ job, don't adopt it
        if (!hasJob(req.jobId) || getJob(req.jobId).isNotInState({ACTIVE, INITIALIZING_TO_ACTIVE})) {

            // Look for an active job that can be suspended
            for (auto it : jobs) {
                Job& job = *it.second;
                // Job must be active and a non-root leaf node
                if (job.isInState({ACTIVE, INITIALIZING_TO_ACTIVE}) 
                    && !job.isRoot() && !job.hasLeftChild() && !job.hasRightChild()) {
                    
                    // Inform parent node of the original job  
                    Console::log(Console::VERB, "Suspending %s ...", job.toStr());
                    Console::log(Console::VERB, "... in order to adopt starving job %s", 
                                    jobStr(req.jobId, req.requestedNodeIndex));  
                    IntPair pair(it.first, job.getIndex());
                    MyMpi::isend(MPI_COMM_WORLD, it.second->getParentNodeRank(), MSG_WORKER_DEFECTING, pair);
                    stats.increment("sentMessages");

                    // Suspend this job
                    job.suspend();
                    setLoad(0, job.getId());

                    adopts = true;
                    break;
                }
            }
        }
    } 
    
    if (adopts) {
        // Adoption takes place
        const char* jobstr = jobStr(req.jobId, req.requestedNodeIndex);
        Console::log_recv(Console::INFO, handle->source, "Willing to adopt %s after %i bounces", jobstr, req.numHops);
        assert(isIdle() || Console::fail("Adopting a job, but not idle!"));
        stats.push_back("bounces", req.numHops);

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
        stats.increment("sentMessages");

    } else {
        // Continue job finding procedure somewhere else
        bounceJobRequest(req);
    }
}

void Worker::handleRequestBecomeChild(MessageHandlePtr& handle) {

    JobRequest req; req.deserialize(*handle->recvData);
    Console::log_recv(Console::VERB, handle->source, "Request to become parent of %s", 
                    jobStr(req.jobId, req.requestedNodeIndex));

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

    } else {
        // Adopt the job
        // TODO Check if this node already has the full description!
        const JobDescription& desc = job.getDescription();

        // Send job signature
        JobSignature sig(req.jobId, req.rootRank, desc.getTransferSize());
        MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_ACCEPT_BECOME_CHILD, sig);
        stats.increment("sentMessages");

        // If req.fullTransfer, then wait for the child to acknowledge having received the signature
        if (req.fullTransfer == 1) {
            Console::log_send(Console::INFO, handle->source, "Sending %s", jobStr(req.jobId, req.requestedNodeIndex));
        } else {
            Console::log_send(Console::INFO, handle->source, "Resuming child %s", jobStr(req.jobId, req.requestedNodeIndex));

            // No further communication required -- child will resume its job solvers
            // Mark new node as one of the node's children
            if (req.requestedNodeIndex == jobs[req.jobId]->getLeftChildIndex()) {
                jobs[req.jobId]->setLeftChild(handle->source);
            } else if (req.requestedNodeIndex == jobs[req.jobId]->getRightChildIndex()) {
                jobs[req.jobId]->setRightChild(handle->source);
            } else assert(req.requestedNodeIndex == 0);
        }
    }

    // If rejected: Send message to rejected child node
    if (reject) {
        Console::log_send(Console::VERB, handle->source, "Rejecting potential child %s", jobStr(req.jobId, req.requestedNodeIndex));
        MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_REJECT_BECOME_CHILD, req);
        stats.increment("sentMessages");
    }
}

void Worker::handleRejectBecomeChild(MessageHandlePtr& handle) {

    // Retrieve committed job
    JobRequest req; req.deserialize(*handle->recvData);
    assert(hasJob(req.jobId));
    Job &job = getJob(req.jobId);
    assert(job.getState() == JobState::COMMITTED);

    // Erase commitment
    Console::log_recv(Console::INFO, handle->source, "Rejected to become %s : uncommitting", job.toStr());
    jobCommitments.erase(req.jobId);
    job.uncommit(req);
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
        stats.increment("sentMessages");
        MyMpi::irecv(MPI_COMM_WORLD, handle->source, MSG_SEND_JOB, sig.getTransferSize()); // to be received later
        stats.increment("receivedMessages");

    } else {
        // Already has job description: Directly resume job (if not terminated yet)
        assert(hasJob(req.jobId));
        Job& job = getJob(req.jobId);
        if (job.getState() != JobState::PAST) {
            Console::log_recv(Console::INFO, handle->source, "Starting or resuming %s (state: %s)", 
                        jobStr(req.jobId, req.requestedNodeIndex), job.jobStateToStr());
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
    MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_SEND_JOB, job.getSerializedDescription());
    stats.increment("sentMessages");
    Console::log_send(Console::VERB, handle->source, "Sent full job description of %s", job.toStr());

    if (job.getState() == JobState::PAST) {
        // Job already terminated -- send termination signal
        IntPair pair(req.jobId, req.requestedNodeIndex);
        MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_TERMINATE, pair);
        stats.increment("sentMessages");
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
            stats.increment("sentMessages");
        }
    }
}

void Worker::handleSendJob(MessageHandlePtr& handle) {
    int jobId; memcpy(&jobId, handle->recvData->data(), sizeof(int));
    assert(hasJob(jobId) || Console::fail("I don't know job #%i !", jobId));

    // Erase job commitment
    if (jobCommitments.count(jobId))
        jobCommitments.erase(jobId);

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
    
    // Initialize job
    job.initialize();
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
    job.communicate(handle->source, msg);
}

void Worker::handleWorkerFoundResult(MessageHandlePtr& handle) {

    // Retrieve job
    int jobId; memcpy(&jobId, handle->recvData->data(), sizeof(int));
    assert(hasJob(jobId) && getJob(jobId).isRoot());
    Console::log_recv(Console::VERB, handle->source, "Result has been found for job #%i", jobId);
    if (getJob(jobId).isInState({PAST, INITIALIZING_TO_PAST})) {
        Console::log_recv(Console::VERB, handle->source, "Discarding excess result for job #%i", jobId);
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
        stats.increment("sentMessages");
        Console::log_send(Console::VERB, handle->source, "Sending client rank (%i)", payload.second); 
    }

    // Terminate job and propagate termination message
    handleTerminate(handle);
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
    stats.increment("sentMessages");
}

void Worker::handleTerminate(MessageHandlePtr& handle) {

    int jobId; memcpy(&jobId, handle->recvData->data(), sizeof(int));
    Job& job = getJob(jobId);

    if (job.isInState({COMMITTED})) {
        Console::log(Console::INFO, "Deferring termination handle, as job description did not arrive yet");
        MyMpi::deferHandle(handle); // do not consume this message while job state is "COMMITTED"
    }

    if (job.isInState({ACTIVE, INITIALIZING_TO_ACTIVE})) {

        // Propagate termination message down the job tree
        if (job.hasLeftChild()) {
            MyMpi::isend(MPI_COMM_WORLD, job.getLeftChildNodeRank(), MSG_TERMINATE, handle->recvData);
            stats.increment("sentMessages");
        }
        if (job.hasRightChild()) {
            MyMpi::isend(MPI_COMM_WORLD, job.getRightChildNodeRank(), MSG_TERMINATE, handle->recvData);
            stats.increment("sentMessages");
        }

        // Terminate
        Console::log(Console::INFO, "Terminating %s", job.toStr());
        setLoad(0, job.getId());
        job.withdraw();
    }
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
        Console::fail("%s : unknown child %s is defecting to another node", job.toStr(), jobStr(jobId, index));
    }
    int nextNodeRank = getRandomWorkerNode();

    // Initiate search for a replacement for the defected child
    Console::log(Console::VERB, "%s : trying to find a new child replacing defected node %s", 
                    job.toStr(), jobStr(jobId, index));
    JobRequest req(jobId, job.getRootNodeRank(), worldRank, index, epochCounter.getEpoch(), 0);
    MyMpi::isend(MPI_COMM_WORLD, nextNodeRank, MSG_FIND_NODE, req);
    stats.increment("sentMessages");
}

void Worker::informClient(int jobId, int clientRank) {
    const JobResult& result = getJob(jobId).getResult();

    // Send "Job done!" with advertised result size to client
    Console::log_send(Console::VERB, clientRank, "Sending JOB_DONE to client");
    IntPair payload(jobId, result.getTransferSize());
    MyMpi::isend(MPI_COMM_WORLD, clientRank, MSG_JOB_DONE, payload);
    stats.increment("sentMessages");
}

void Worker::setLoad(int load, int whichJobId) {
    assert(load + this->load == 1); // (load WAS 1) XOR (load BECOMES 1)
    this->load = load;

    // Measure time for which worker was {idle,busy}
    float now = Timer::elapsedSeconds();
    stats.add((load == 0 ? "busyTime" : "idleTime"), now - lastLoadChange);
    lastLoadChange = now;
    assert(hasJob(whichJobId));
    if (load == 1)
        Console::log(Console::VERB, "LOAD 1 (+%s)", getJob(whichJobId).toStr());
    if (load == 0)
        Console::log(Console::VERB, "LOAD 0 (-%s)", getJob(whichJobId).toStr());
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

void Worker::bounceJobRequest(JobRequest& request) {

    // Increment #hops
    request.numHops++;
    stats.increment("bouncedJobs");
    int num = request.numHops;

    // Show warning if #hops is a large power of two
    if ((num >= 512) && ((num & (num - 1)) == 0)) {
        Console::log(Console::WARN, "%s bouncing for the %i. time", jobStr(request.jobId, request.requestedNodeIndex), num);
    }

    // Generate pseudorandom permutation of this request
    int n = MyMpi::size(comm);
    AdjustablePermutation perm(n, 3 * request.jobId + 7 * request.requestedNodeIndex + 11 * request.requestingNodeRank);
    // Fetch next index of permutation based on number of hops
    int permIdx = request.numHops % n;
    int nextRank = perm.get(permIdx);
    // (while skipping yourself and the requesting node)
    while (nextRank == worldRank || nextRank == request.requestingNodeRank) {
        permIdx = (permIdx+1) % n;
        nextRank = perm.get(permIdx);
    }

    // Send request to a random other worker node
    //int nextRank = getRandomWorkerNode();
    Console::log_send(Console::VVVERB, nextRank, "Bouncing %s", jobStr(request.jobId, request.requestedNodeIndex));
    MyMpi::isend(MPI_COMM_WORLD, nextRank, MSG_FIND_NODE, request);
    stats.increment("sentMessages");
}

void Worker::rebalance() {

    // Initiate balancing procedure
    bool done = balancer->beginBalancing(jobs);
    // If nothing to do, finish up balancing
    if (done) finishBalancing();
}

void Worker::finishBalancing() {
    // All collective operations are done; reset synchronized timer
    epochCounter.resetLastSync();

    // Retrieve balancing results
    Console::log(Console::VERB, "Finishing balancing ...");
    std::map<int, int> volumes = balancer->getBalancingResult();
    Console::log(MyMpi::rank(comm) == 0 ? Console::INFO : Console::VERB, "Rebalancing completed.");

    // Add last slice of idle/busy time 
    stats.add((load == 1 ? "busyTime" : "idleTime"), Timer::elapsedSeconds() - lastLoadChange);
    lastLoadChange = Timer::elapsedSeconds();

    // Dump stats and advance to next epoch
    stats.addResourceUsage();
    stats.dump(epochCounter.getEpoch());
    for (auto it : jobs) {
        it.second->dumpStats();
    }
    epochCounter.increment();
    Console::log(Console::VERB, "Advancing to epoch %i", epochCounter.getEpoch());
    
    // Update volumes found during balancing, and trigger job expansions / shrinkings
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
        stats.increment("sentMessages");
        if (nextIndex >= volume) {
            // Prune child
            Console::log_send(Console::VERB, job.getLeftChildNodeRank(), "Pruning left child of %s", job.toStr());
            job.unsetLeftChild();
        }
    } else if (job.hasJobDescription() && nextIndex < volume) {
        // Grow left
        JobRequest req(jobId, job.getRootNodeRank(), worldRank, nextIndex, epochCounter.getEpoch(), 0);
        int nextNodeRank = job.getLeftChildNodeRank();
        MyMpi::isend(MPI_COMM_WORLD, nextNodeRank, MSG_FIND_NODE, req);
        stats.increment("sentMessages");
    }

    // Right child
    nextIndex = job.getRightChildIndex();
    if (job.hasRightChild()) {
        // Propagate right
        MyMpi::isend(MPI_COMM_WORLD, job.getRightChildNodeRank(), MSG_UPDATE_VOLUME, payload);
        stats.increment("sentMessages");
        if (nextIndex >= volume) {
            // Prune child
            Console::log_send(Console::VERB, job.getRightChildNodeRank(), "Pruning right child of %s", job.toStr());
            job.unsetRightChild();
        }
    } else if (job.hasJobDescription() && nextIndex < volume) {
        // Grow right
        JobRequest req(jobId, job.getRootNodeRank(), worldRank, nextIndex, epochCounter.getEpoch(), 0);
        int nextNodeRank = job.getRightChildNodeRank();
        MyMpi::isend(MPI_COMM_WORLD, nextNodeRank, MSG_FIND_NODE, req);
        stats.increment("sentMessages");
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

float Worker::allReduce(float contribution) {
    float result;
    MPI_Allreduce(&contribution, &result, 1, MPI_FLOAT, MPI_SUM, comm);
    stats.increment("reductions");
    stats.increment("broadcasts");
    return result;
}

float Worker::reduce(float contribution, int rootRank) {
    float result;
    MPI_Reduce(&contribution, &result, 1, MPI_FLOAT, MPI_SUM, rootRank, comm);
    stats.increment("reductions");
    return result;
}
