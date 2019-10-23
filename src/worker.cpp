
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
    MyMpi::listen();
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

        if (!balancer->isBalancing() && isTimeForRebalancing()) {
            
            checkTerminate();

            // Dump stats and advance epoch
            stats.add((load == 1 ? "busyTime" : "idleTime"), Timer::elapsedSeconds() - lastLoadChange);
            lastLoadChange = Timer::elapsedSeconds();
            int epoch = epochCounter.getEpoch();
            stats.dump(epoch);

            // Rebalancing
            Console::log(MyMpi::rank(comm) == 0 ? Console::INFO : Console::VERB, "Entering rebalancing of epoch %i", epoch);
            /*int numOccupiedNodes = allReduce(isIdle() ? 0.0f : 1.0f);
            if (MyMpi::rank(comm) == 0) {
                Console::log(Console::INFO, "Before rebalancing: %i occupied nodes", numOccupiedNodes);
            }*/
            rebalance();

        } else if (balancer->isBalancing()) {
            if (balancer->canContinueBalancing()) {
                bool done = balancer->continueBalancing();
                if (done) finishBalancing();
            }
        }

        // Clause sharing
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
                std::vector<int> payload;
                payload.push_back(jobId); payload.push_back(result);
                // May be a self message!
                Console::log_send(Console::VERB, jobRootRank, "Sending finished info");
                MyMpi::isend(MPI_COMM_WORLD, jobRootRank, MSG_WORKER_FOUND_RESULT, payload);
                stats.increment("sentMessages");
            }
        }

        // Sleep for a bit
        //usleep(100); // 1000 = 1 millisecond

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
            
            else if (handle->tag == MSG_COLLECTIVES) {
                bool done = balancer->handleMessage(handle);
                if (done) finishBalancing();

            } else {
                Console::log_recv(Console::WARN, handle->source, "Unknown message tag %i", handle->tag);
            }

            // Listen to another message, if no critical listener is active
            MyMpi::listen();
        }
    }
}

void Worker::handleFindNode(MessageHandlePtr& handle) {

    JobRequest req; req.deserialize(handle->recvData);

    if (req.iteration != (int)epochCounter.getEpoch() && req.requestedNodeIndex > 0) {
        Console::log_recv(Console::INFO, handle->source, "Discarding a job request from a previous iteration");
        return;
    }

    if (hasJob(req.jobId) && jobs[req.jobId]->getState() == JobState::PAST) {
        // This job already finished!
        Console::log(Console::INFO, "Consuming request %s as it already finished", jobStr(req.jobId, req.requestedNodeIndex));
        return;
    }

    if (isIdle() && !hasJobCommitments()) {

        const char* jobstr = jobStr(req.jobId, req.requestedNodeIndex);
        Console::log_recv(Console::INFO, handle->source, "Willing to adopt %s after %i bounces", jobstr, req.numHops);
        stats.push_back("bounces", req.numHops);

        // Commit on the job, send a request to the parent
        bool fullTransfer = false;
        if (!hasJob(req.jobId)) {
            // Job is not known yet
            jobs[req.jobId] = new SatJob(params, MyMpi::size(comm), worldRank, req.jobId, epochCounter);
            fullTransfer = true;
        }
        req.fullTransfer = fullTransfer ? 1 : 0;
        jobs[req.jobId]->commit(req);
        jobCommitments[req.jobId] = req;
        MyMpi::isend(MPI_COMM_WORLD, req.requestingNodeRank, MSG_REQUEST_BECOME_CHILD, jobCommitments[req.jobId]);
        stats.increment("sentMessages");

    } else {
        // Continue job finding procedure
        Console::log_recv(Console::VVVERB, handle->source, "Bouncing %s", jobStr(req.jobId, req.requestedNodeIndex));
        bounceJobRequest(req);
    }
}

void Worker::handleRequestBecomeChild(MessageHandlePtr& handle) {

    Console::log_recv(Console::VERB, handle->source, "Request to become parent");
    JobRequest req; req.deserialize(handle->recvData);

    // Retrieve concerned job
    assert(hasJob(req.jobId));
    Job &job = getJob(req.jobId);

    if (req.iteration != (int)epochCounter.getEpoch() && req.requestedNodeIndex == 0) {
        // Looking for a root node => Does not become obsolet
        Console::log(Console::INFO, "Updating epoch of root job request #%i:0", req.jobId);
        req.iteration = epochCounter.getEpoch();
    }

    bool reject = false;
    if (req.iteration != (int)epochCounter.getEpoch()) {

        Console::log_recv(Console::INFO, handle->source, "Discarding request %s from epoch %i (now is epoch %i)", 
                            job.toStr(), req.iteration, epochCounter.getEpoch());
        reject = true;

    } else if (job.isNotInState({ACTIVE, STORED, INITIALIZING_TO_ACTIVE})) {

        Console::log_recv(Console::INFO, handle->source, "%s is not active and not stored (any more) -- discarding", job.toStr());
        Console::log(Console::VERB, "Actual job state: %s", job.jobStateToStr());
        reject = true;

    } else {

        const JobDescription& desc = job.getDescription();

        // Send job signature
        JobSignature sig(req.jobId, req.rootRank, desc.getPayloadSize());
        MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_ACCEPT_BECOME_CHILD, sig);
        stats.increment("sentMessages");

        // If req.fullTransfer, then wait for the child to acknowledge having received the signature
        if (req.fullTransfer == 1) {
            Console::log_send(Console::INFO, handle->source, "Sending %s", jobStr(req.jobId, req.requestedNodeIndex));
        } else {
            Console::log_send(Console::INFO, handle->source, "Resuming child %s", jobStr(req.jobId, req.requestedNodeIndex));

            // Immediately mark new node as one of the node's children, if applicable
            if (req.requestedNodeIndex == jobs[req.jobId]->getLeftChildIndex()) {
                jobs[req.jobId]->setLeftChild(handle->source);
            } else if (req.requestedNodeIndex == jobs[req.jobId]->getRightChildIndex()) {
                jobs[req.jobId]->setRightChild(handle->source);
            } else assert(req.requestedNodeIndex == 0);
        }
    }

    if (reject) {
        MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_REJECT_BECOME_CHILD, req);
        stats.increment("sentMessages");
    }
}

void Worker::handleRejectBecomeChild(MessageHandlePtr& handle) {

    JobRequest req; req.deserialize(handle->recvData);
    assert(hasJob(req.jobId));
    Job &job = getJob(req.jobId);
    assert(job.getState() == JobState::COMMITTED);

    Console::log_recv(Console::INFO, handle->source, "Commitment of %s broken -- uncommitting", job.toStr());
    jobCommitments.erase(req.jobId);
    job.uncommit(req);
}

void Worker::handleAcceptBecomeChild(MessageHandlePtr& handle) {

    JobSignature sig; sig.deserialize(handle->recvData);
    assert(jobCommitments.count(sig.jobId));
    JobRequest& req = jobCommitments[sig.jobId];

    if (req.fullTransfer == 1) {
        Console::log(Console::VERB, "Receiving job description of #%i of size %i", req.jobId, sig.getTransferSize());
        MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_ACK_ACCEPT_BECOME_CHILD, req);
        stats.increment("sentMessages");
        MyMpi::irecv(MPI_COMM_WORLD, handle->source, MSG_SEND_JOB, sig.getTransferSize()); // to be received later
        stats.increment("receivedMessages");

    } else {
        assert(hasJob(req.jobId));
        Job& job = getJob(req.jobId);
        if (job.getState() == JobState::PAST) {
            Console::log(Console::WARN, "%s already finished, so it will not be re-initialized", job.toStr());
        } else {
            Console::log_recv(Console::INFO, handle->source, "Starting or resuming %s (state: %s)", jobStr(req.jobId, req.requestedNodeIndex), job.jobStateToStr());
            setLoad(1);
            job.reinitialize(req.requestedNodeIndex, req.rootRank, req.requestingNodeRank);
        }
    }

    jobCommitments.erase(sig.jobId);
}

void Worker::handleAckAcceptBecomeChild(MessageHandlePtr& handle) {
    JobRequest req; req.deserialize(handle->recvData);

    // Retrieve and send concerned job
    assert(hasJob(req.jobId));
    Job& job = getJob(req.jobId);
    MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_SEND_JOB, job.getSerializedDescription());
    stats.increment("sentMessages");
    Console::log_send(Console::VERB, handle->source, "Sent full job description of %s", job.toStr());

    if (job.getState() == JobState::PAST) {
        // Job already terminated -- send termination signal
        MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_TERMINATE, req.jobId);
        stats.increment("sentMessages");
    } else {

        // Mark new node as one of the node's children, if applicable
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
            std::vector<int> jobIdAndVolume;
            jobIdAndVolume.push_back(req.jobId);
            jobIdAndVolume.push_back(volume);
            MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_UPDATE_VOLUME, jobIdAndVolume);
            stats.increment("sentMessages");
        }
    }
}

void Worker::handleSendJob(MessageHandlePtr& handle) {
    int jobId = handle->recvData[0];
    setLoad(1);
    assert(hasJob(jobId));
    Console::log(Console::VERB, "Received full job description of #%i. Initializing ...", jobId);
    initializerThreads[jobId] = std::thread(&Worker::initJob, this, handle);
}

void Worker::initJob(MessageHandlePtr handle) {
    int jobId = handle->recvData[0];
    Console::log_recv(Console::VERB, handle->source, "Deserializing job #%i ...", jobId);
    Job& job = getJob(jobId);
    job.setDescription(handle->recvData);
    job.beginInitialization();
    job.initialize();
}

void Worker::handleUpdateVolume(MessageHandlePtr& handle) {
    int jobId = handle->recvData[0];
    int volume = handle->recvData[1];
    if (!hasJob(jobId)) {
        Console::log(Console::WARN, "WARN: Received a volume update about #%i, which is unknown to me", jobId);
        return;
    }
    balancer->updateVolume(jobId, volume);
    updateVolume(jobId, volume);
}

void Worker::handleJobCommunication(MessageHandlePtr& handle) {
    JobMessage msg; msg.deserialize(handle->recvData);
    int jobId = msg.jobId;
    if (!hasJob(jobId)) {
        Console::log(Console::WARN, "Job message from unknown job #%i", jobId);
        return;
    }
    Job& job = getJob(jobId);
    job.communicate(handle->source, msg);
}

void Worker::handleWorkerFoundResult(MessageHandlePtr& handle) {

    int jobId = handle->recvData[0];
    assert(hasJob(jobId) && getJob(jobId).isRoot());
    std::vector<int> payload;
    payload.push_back(jobId);
    payload.push_back(getJob(jobId).getParentNodeRank());
    Console::log_recv(Console::VERB, handle->source, "Result has been found for job #%i", jobId);

    if (handle->source == worldRank) {
        // Self-message of root node: Directly send termination message to client
        informClient(payload[0], payload[1]);
    } else {
        // Send rank of client node to the worker which finished,
        // such that the worker can directly inform the client
        MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_FORWARD_CLIENT_RANK, payload);
        stats.increment("sentMessages");
        Console::log_send(Console::VERB, handle->source, "Sending client rank (%i)", payload[1]); 
    }

    handleTerminate(handle);
}

void Worker::handleForwardClientRank(MessageHandlePtr& handle) {

    // Receive rank of the job's client
    int jobId = handle->recvData[0];
    int clientRank = handle->recvData[1];
    assert(hasJob(jobId));
    informClient(jobId, clientRank);    
}

void Worker::informClient(int jobId, int clientRank) {
    const JobResult& result = getJob(jobId).getResult();

    // Send "Job done!" with advertised result size to client
    Console::log_send(Console::VERB, clientRank, "Sending JOB_DONE to client");
    std::vector<int> payload;
    payload.push_back(jobId);
    payload.push_back(result.getTransferSize());
    MyMpi::isend(MPI_COMM_WORLD, clientRank, MSG_JOB_DONE, payload);
    stats.increment("sentMessages");
}

void Worker::handleQueryJobResult(MessageHandlePtr& handle) {

    // Receive acknowledgement that the client received the advertised result size
    // and wishes to receive the full job result
    int jobId = handle->recvData[0];
    assert(hasJob(jobId));
    const JobResult& result = getJob(jobId).getResult();
    Console::log_send(Console::VERB, handle->source, "Sending full job result to client");
    MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_SEND_JOB_RESULT, result);
    stats.increment("sentMessages");
}

void Worker::handleTerminate(MessageHandlePtr& handle) {

    int jobId = handle->recvData[0];
    Job& job = getJob(jobId);

    if (job.getState() == JobState::COMMITTED) {
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
        setLoad(0);
        job.withdraw();
    }
}

void Worker::setLoad(int load) {
    assert(load + this->load == 1);
    this->load = load;
    float now = Timer::elapsedSeconds();
    stats.add((load == 0 ? "busyTime" : "idleTime"), now - lastLoadChange);
    lastLoadChange = now;
}

int Worker::getRandomWorkerNode() {

    std::set<int> excludedNodes = std::set<int>(this->clientNodes);
    excludedNodes.insert(worldRank);
    int randomOtherNodeRank = MyMpi::random_other_node(MPI_COMM_WORLD, excludedNodes);
    return randomOtherNodeRank;
}

void Worker::bounceJobRequest(JobRequest& request) {

    request.numHops++;
    stats.increment("bouncedJobs");
    int num = request.numHops;
    if ((num >= 512) && ((num & (num - 1)) == 0)) {
        Console::log(Console::WARN, "%s bouncing for the %i. time", jobStr(request.jobId, request.requestedNodeIndex), num);
    }
    int randomOtherNodeRank = getRandomWorkerNode();
    MyMpi::isend(MPI_COMM_WORLD, randomOtherNodeRank, MSG_FIND_NODE, request);
    stats.increment("sentMessages");
}

void Worker::rebalance() {
    bool done = balancer->beginBalancing(jobs);
    if (done) finishBalancing();
}

void Worker::finishBalancing() {
    Console::log(Console::VERB, "Finishing balancing ...");
    std::map<int, int> volumes = balancer->getBalancingResult();

    if (MyMpi::rank(comm) == 0)
        Console::log(Console::INFO, "Rebalancing completed.");

    epochCounter.increment();
    Console::log(Console::VERB, "Entering epoch %i", epochCounter.getEpoch());
    
    for (auto it = volumes.begin(); it != volumes.end(); ++it) {
        Console::log(Console::INFO, "Job #%i : new volume %i", it->first, it->second);
        updateVolume(it->first, it->second);
    }

    // All collective operations are done; reset synchronized timer
    epochCounter.resetLastSync();
}

void Worker::updateVolume(int jobId, int volume) {

    Job &job = getJob(jobId);

    if (job.isNotInState({ACTIVE, INITIALIZING_TO_ACTIVE})) {
        // Job is not active right now
        return;
    }

    // Prepare volume update to propagate down the job tree
    std::vector<int> payload;
    payload.push_back(jobId);
    payload.push_back(volume);

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
    } else if (nextIndex < volume) {
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
    } else if (nextIndex < volume) {
        // Grow right
        JobRequest req(jobId, job.getRootNodeRank(), worldRank, nextIndex, epochCounter.getEpoch(), 0);
        int nextNodeRank = job.getRightChildNodeRank();
        MyMpi::isend(MPI_COMM_WORLD, nextNodeRank, MSG_FIND_NODE, req);
        stats.increment("sentMessages");
    }

    // Shrink (and pause solving) if necessary
    if (thisIndex > 0 && thisIndex >= volume) {
        setLoad(0);
        jobs[jobId]->suspend();
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
