
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
    balancer = std::unique_ptr<Balancer>(new CutoffPriorityBalancer(comm, params));
    
    // Begin listening to an incoming message
    MyMpi::irecv(MPI_COMM_WORLD);
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

    Console::log(Console::INFO, "Worker node set up.");

    while (true) {

        if (isTimeForRebalancing()) {

            checkTerminate();

            // Rebalancing
            Console::log(MyMpi::rank(comm) == 0 ? Console::INFO : Console::VERB, "Entering rebalancing");
            int numOccupiedNodes = allReduce(isIdle() ? 0.0f : 1.0f);
            if (MyMpi::rank(comm) == 0) {
                Console::log(Console::INFO, "Before rebalancing: " + std::to_string(numOccupiedNodes) + " occupied nodes");
            }
            rebalance();
        }

        // Clause sharing
        for (auto it : jobs) {
            int jobId = it.first;
            Job& job = *it.second;
            if (job.wantsToCommunicate()) {
                Console::log(Console::VERB, job.toStr() + " wants to communicate");
                job.communicate();
            }
        }

        // Solve loops for each active HordeLib instance
        for (auto it : jobs) {
            int jobId = it.first;
            Job &job = *it.second;
            if (job.getState() != JobState::ACTIVE)
                continue;
            int result = job.solveLoop();
            if (result >= 0) {
                // Solver done!
                int jobRootRank = job.getRootNodeRank();
                std::vector<int> payload;
                payload.push_back(jobId); payload.push_back(result);
                Console::log_send(Console::VERB, "Sending finished info", jobRootRank);
                MyMpi::isend(MPI_COMM_WORLD, jobRootRank, MSG_WORKER_FOUND_RESULT, payload);
            }
        }

        // Sleep for a bit
        usleep(1000); // 1000 = 1 millisecond

        // Poll messages, if present
        MessageHandlePtr handle;
        if ((handle = MyMpi::poll()) != NULL) {
            // Process message
            Console::log_recv(Console::VVERB, "Processing message of tag " + std::to_string(handle->tag), handle->source);

            if (handle->tag == MSG_INTRODUCE_JOB)
                handleIntroduceJob(handle);

            else if (handle->tag == MSG_FIND_NODE)
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
            
            else {
                Console::log_recv(Console::WARN, "Unknown message tag " + std::to_string(handle->tag) + "!", handle->source);
            }

            /*
            // Adjustments of node permutations
            if (handle->tag == MSG_CHECK_NODE_PERMUTATION) {
                int jobId = handle->recvData[0];
                int index = handle->recvData[1];
                // Check if #jobId:index if and where bounced off from here
                if (computes(jobId) && jobIndices[jobId] == index) {
                    // Permutation is correct - this is the node
                    MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_CONFIRM_NODE_PERMUTATION, handle->recvData);
                } else {
                    // Job bounced off or never was there - link to other node
                    if (!jobNodes.count(jobId)) {
                        jobNodes[jobId] = AdjustablePermutation(MyMpi::size(comm), jobId);
                    }
                    handle->recvData.push_back(jobNodes[jobId].get(index));
                    MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_ADJUST_NODE_PERMUTATION, handle->recvData);
                }
            }
            if (handle->tag == MSG_ADJUST_NODE_PERMUTATION) {
                int jobId = handle->recvData[0];
                int index = handle->recvData[1];
                int actualNode = handle->recvData[2];
                if (!jobNodes.count(jobId)) {
                    jobNodes[jobId] = AdjustablePermutation(MyMpi::size(comm), jobId);
                }
                jobNodes[jobId].adjust(index, actualNode);
                if (actualNode == handle->source) {
                    // TODO Job is not on the called node right now, but should / might be later
                }
            }
            if (handle->tag == MSG_CONFIRM_NODE_PERMUTATION) {
                int jobId = handle->recvData[0];
                int prevIndex = handle->recvData[1];
                // TODO "check off" this index
            }
            */

            // Listen to another message, if no listener is active
            if (!MyMpi::hasActiveHandles())
                MyMpi::irecv(MPI_COMM_WORLD);
        }
    }
}

void Worker::handleIntroduceJob(MessageHandlePtr& handle) {

    // Receive job signature and then the actual job
    JobSignature sig; sig.deserialize(handle->recvData);
    MessageHandlePtr jobHandle = MyMpi::recv(MPI_COMM_WORLD, MSG_SEND_JOB, sig.getTransferSize()); // BLOCKING
    JobDescription desc; desc.deserialize(jobHandle->recvData);

    assert(!hasJob(desc.getId()));
    Job *job = new SatJob(params, MyMpi::size(comm), worldRank, desc.getId(), epochCounter);
    jobs[desc.getId()] = job;
    job->store(desc);

    if (isIdle() && !hasJobCommitments()) {
        // Accept and initialize the job
        Console::log_recv(Console::INFO, "Job #" + std::to_string(desc.getId()) + " introduced. Beginning to compute as root node", jobHandle->source);
        job->initialize(/*index=*/0, /*rootRank=*/worldRank, /*parentRank=*/handle->source);
        load = 1;

    } else {
        // Trigger a node finding procedure
        Console::log_recv(Console::INFO, "Job #" + std::to_string(desc.getId()) + " introduced. Bouncing ...", handle->source);
        JobRequest request(desc.getId(), worldRank, handle->source, 0, epochCounter.getEpoch(), -1);
        bounceJobRequest(request);
    }
}

void Worker::handleFindNode(MessageHandlePtr& handle) {

    JobRequest req; req.deserialize(handle->recvData);

    if (req.iteration != epochCounter.getEpoch() && req.requestedNodeIndex > 0) {
        Console::log_recv(Console::INFO, "Discarding a job request from a previous iteration", handle->source);
        return;
    }

    if (hasJob(req.jobId) && jobs[req.jobId]->getState() == JobState::PAST) {
        // This job already finished!
        Console::log(Console::INFO, "Consuming request " + jobStr(req.jobId, req.requestedNodeIndex)
                   + " as it already finished");
        return;
    }

    if (isIdle() && !hasJobCommitments()) {

        Console::log_recv(Console::INFO, "Willing to adopt " + jobStr(req.jobId, req.requestedNodeIndex)
                        + " after " + std::to_string(req.numHops) + " bounces", handle->source);

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
        MyMpi::isend(MPI_COMM_WORLD, (req.requestedNodeIndex == 0 ? req.rootRank : req.requestingNodeRank), 
            MSG_REQUEST_BECOME_CHILD, jobCommitments[req.jobId]);

    } else {
        // Continue job finding procedure
        Console::log_recv(Console::VVERB, "Bouncing " + jobStr(req.jobId, req.requestedNodeIndex), handle->source);
        bounceJobRequest(req);
    }
}

void Worker::handleRequestBecomeChild(MessageHandlePtr& handle) {

    Console::log_recv(Console::VERB, "Request to become parent", handle->source);
    JobRequest req; req.deserialize(handle->recvData);

    // Retrieve concerned job
    assert(hasJob(req.jobId));
    Job &job = getJob(req.jobId);

    if (req.iteration != epochCounter.getEpoch() && req.requestedNodeIndex == 0) {
        // Looking for a root node => Does not become obsolet
        Console::log(Console::INFO, "Updating epoch of root job request #" + std::to_string(req.jobId) + ":0");
        req.iteration = epochCounter.getEpoch();
    }

    bool reject = false;
    if (req.iteration != epochCounter.getEpoch()) {

        Console::log_send(Console::INFO, "Discarding request " + job.toStr() + " from epoch " 
            + std::to_string(req.iteration) + " (now is " + std::to_string(epochCounter.getEpoch()) + ")", handle->source);
        reject = true;

    } else if (job.getState() != JobState::ACTIVE 
            && job.getState() != JobState::STORED) {

        Console::log_send(Console::INFO, job.toStr() + " is not active and not stored (any more) -- discarding", handle->source);
        Console::log(Console::VERB, "Actual job state: " + job.jobStateToStr());
        reject = true;

    } else {

        const JobDescription& desc = job.getDescription();

        // Send job signature
        JobSignature sig(req.jobId, req.rootRank, desc.getPayloadSize());
        MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_ACCEPT_BECOME_CHILD, sig);

        // If req.fullTransfer, then wait for the child to acknowledge having received the signature
        if (req.fullTransfer == 1) {
            Console::log_send(Console::INFO, "Sending " + jobStr(req.jobId, req.requestedNodeIndex), handle->source);
        } else {
            Console::log_send(Console::INFO, "Resuming child " + jobStr(req.jobId, req.requestedNodeIndex), handle->source);

            // Immediately mark new node as one of the node's children, if applicable
            if (req.requestedNodeIndex == jobs[req.jobId]->getLeftChildIndex()) {
                jobs[req.jobId]->setLeftChild(handle->source);
            } else if (req.requestedNodeIndex == jobs[req.jobId]->getRightChildIndex()) {
                jobs[req.jobId]->setRightChild(handle->source);
            } else assert(req.requestedNodeIndex == 0);
        }
    }

    if (reject)
        MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_REJECT_BECOME_CHILD, req);
}

void Worker::handleRejectBecomeChild(MessageHandlePtr& handle) {

    JobRequest req; req.deserialize(handle->recvData);
    assert(hasJob(req.jobId));
    Job &job = getJob(req.jobId);
    assert(job.getState() == JobState::COMMITTED);

    Console::log_recv(Console::INFO, "Commitment of " + job.toStr() + " broken -- uncommitting", handle->source);
    jobCommitments.erase(req.jobId);
    job.uncommit(req);
}

void Worker::handleAcceptBecomeChild(MessageHandlePtr& handle) {

    JobSignature sig; sig.deserialize(handle->recvData);
    assert(jobCommitments.count(sig.jobId));
    JobRequest& req = jobCommitments[sig.jobId];

    if (req.fullTransfer == 1) {
        MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_ACK_ACCEPT_BECOME_CHILD, req);
        MyMpi::irecv(MPI_COMM_WORLD, handle->source, MSG_SEND_JOB, sig.getTransferSize()); // to be received later
        // At this point, the node listens to nothing else except for the job transfer!
        // TODO check possible deadlocks
    } else {
        assert(hasJob(req.jobId));
        Job& job = getJob(req.jobId);
        if (job.getState() == JobState::PAST) {
            Console::log(Console::WARN, job.toStr() + " already finished, so it will not be re-initialized");
        } else {
            Console::log_recv(Console::INFO, "Starting or resuming " + jobStr(req.jobId, req.requestedNodeIndex), handle->source);
            job.reinitialize(req.requestedNodeIndex, req.rootRank, req.requestingNodeRank);
            load = 1;
        }
    }

    jobCommitments.erase(sig.jobId);
}

void Worker::handleAckAcceptBecomeChild(MessageHandlePtr& handle) {
    JobRequest req; req.deserialize(handle->recvData);

    // Retrieve and send concerned job
    assert(hasJob(req.jobId));
    Job& job = getJob(req.jobId);
    const JobDescription &desc = job.getDescription();
    MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_SEND_JOB, desc);
    Console::log_send(Console::VERB, "Sent full job description.", handle->source);

    if (job.getState() == JobState::PAST) {
        // Job already terminated -- send termination signal
        MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_TERMINATE, req.jobId);
    } else {

        // Mark new node as one of the node's children, if applicable
        if (job.isInitialized()) {
            if (req.requestedNodeIndex == job.getLeftChildIndex()) {
                job.setLeftChild(handle->source);
            } else if (req.requestedNodeIndex == job.getRightChildIndex()) {
                job.setRightChild(handle->source);
            } else assert(req.requestedNodeIndex == 0);
        }

        // Send current volume / initial demand update
        if (job.getState() == JobState::ACTIVE) {
            int volume = balancer->getVolume(req.jobId);
            assert(volume >= 1);
            Console::log_send(Console::VERB, "Propagating volume " + std::to_string(volume) + " to new child", handle->source);
            std::vector<int> jobIdAndVolume;
            jobIdAndVolume.push_back(req.jobId);
            jobIdAndVolume.push_back(volume);
            MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_UPDATE_VOLUME, jobIdAndVolume);
        }
    }
}

void Worker::handleSendJob(MessageHandlePtr& handle) {
    JobDescription desc; desc.deserialize(handle->recvData);
    assert(hasJob(desc.getId()));
    Job& job = getJob(desc.getId());
    Console::log(Console::VERB, "Received full job description of " + job.toStr());
    job.store(desc);
    job.initialize();
    load = 1;
}

void Worker::handleUpdateVolume(MessageHandlePtr& handle) {
    int jobId = handle->recvData[0];
    int volume = handle->recvData[1];
    balancer->updateVolume(jobId, volume);
    updateVolume(jobId, volume);
}

void Worker::handleJobCommunication(MessageHandlePtr& handle) {
    JobMessage msg; msg.deserialize(handle->recvData);
    int jobId = msg.jobId;
    if (!hasJob(jobId)) {
        Console::log(Console::WARN, "Job message from unknown job #" + std::to_string(jobId));
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
    Console::log_recv(Console::VERB, "Result has been found for job #" + std::to_string(jobId), handle->source);

    if (getJob(jobId).isRoot()) {
        // Directly send termination message to client
        informClient(payload[0], payload[1]);
    } else {
        // Send rank of client node to the worker which finished,
        // such that the worker can directly inform the client
        MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_FORWARD_CLIENT_RANK, payload);
        Console::log_send(Console::VERB, "Sending client rank (" 
                + std::to_string(payload[1]) + ")", handle->source);
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
    Console::log_send(Console::VERB, "Sending JOB_DONE to client", clientRank);
    std::vector<int> payload;
    payload.push_back(jobId);
    payload.push_back(result.getTransferSize());
    MyMpi::isend(MPI_COMM_WORLD, clientRank, MSG_JOB_DONE, payload);
}

void Worker::handleQueryJobResult(MessageHandlePtr& handle) {

    // Receive acknowledgement that the client received the advertised result size
    // and wishes to receive the full job result
    int jobId = handle->recvData[0];
    assert(hasJob(jobId));
    const JobResult& result = getJob(jobId).getResult();
    Console::log_send(Console::VERB, "Sending full job result to client", handle->source);
    MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_SEND_JOB_RESULT, result);
}

void Worker::handleTerminate(MessageHandlePtr& handle) {

    int jobId = handle->recvData[0];
    Job& job = getJob(jobId);

    if (job.getState() == JobState::COMMITTED) {
        Console::log(Console::INFO, "Deferring termination handle, as job description did not arrive yet");
        MyMpi::deferHandle(handle); // do not consume this message while job state is "COMMITTED"
    }

    // Either this job is still running, or it has been shut down by a previous termination message / rebalancing procedure
    assert(job.getState() == JobState::ACTIVE
        || job.getState() == JobState::PAST
        || job.getState() == JobState::SUSPENDED);

    if (job.getState() == JobState::ACTIVE) {

        // Propagate termination message down the job tree
        if (job.hasLeftChild())
            MyMpi::isend(MPI_COMM_WORLD, job.getLeftChildNodeRank(), MSG_TERMINATE, handle->recvData);
        if (job.hasRightChild())
            MyMpi::isend(MPI_COMM_WORLD, job.getRightChildNodeRank(), MSG_TERMINATE, handle->recvData);

        // Terminate
        Console::log(Console::INFO, "Terminating " + job.toStr());
        job.withdraw();
        load = 0;
    }
}

int Worker::getRandomWorkerNode() {

    std::set<int> excludedNodes = std::set<int>(this->clientNodes);
    excludedNodes.insert(worldRank);
    int randomOtherNodeRank = MyMpi::random_other_node(MPI_COMM_WORLD, excludedNodes);
    return randomOtherNodeRank;
}

void Worker::bounceJobRequest(JobRequest& request) {

    request.numHops++;
    int num = request.numHops;
    if ((num >= 512) && ((num &(num - 1)) == 0)) {
        Console::log(Console::WARN, jobStr(request.jobId, request.requestedNodeIndex) 
            + " bouncing for the " + std::to_string(num) + ". time");
    }
    int randomOtherNodeRank = getRandomWorkerNode();
    MyMpi::isend(MPI_COMM_WORLD, randomOtherNodeRank, MSG_FIND_NODE, request);
}

void Worker::rebalance() {

    epochCounter.increment();
    
    std::map<int, int> volumes = balancer->balance(jobs);

    if (MyMpi::rank(comm) == 0)
        Console::log(Console::INFO, "Rebalancing completed.");

    for (auto it = volumes.begin(); it != volumes.end(); ++it) {
        Console::log(Console::INFO, "Job #" + std::to_string(it->first) + " : new volume " + std::to_string(it->second));
        updateVolume(it->first, it->second);
    }

    // All collective operations are done; reset synchronized timer
    epochCounter.resetLastSync();
}

void Worker::updateVolume(int jobId, int volume) {

    if (!hasJob(jobId)) {
        Console::log(Console::WARN, "WARN: Received a volume update about #"
                + std::to_string(jobId) + ", which is unknown to me.");
        return;
    }

    Job &job = getJob(jobId);

    if (job.getState() != JobState::ACTIVE) {
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
        Console::log(Console::VERB, "Updating volume of " + job.toStr() + " to " + std::to_string(volume));
    }

    // Left child
    int nextIndex = job.getLeftChildIndex();
    if (job.hasLeftChild()) {
        // Propagate left
        MyMpi::isend(MPI_COMM_WORLD, job.getLeftChildNodeRank(), MSG_UPDATE_VOLUME, payload);
        if (nextIndex >= volume) {
            // Prune child
            Console::log_send(Console::VERB, "Pruning left child of " + job.toStr(), job.getLeftChildNodeRank());
            job.unsetLeftChild();
        }
    } else if (nextIndex < volume) {
        // Grow left
        JobRequest req(jobId, job.getRootNodeRank(), worldRank, nextIndex, epochCounter.getEpoch(), 0);
        int nextNodeRank = job.getLeftChildNodeRank();
        MyMpi::isend(MPI_COMM_WORLD, nextNodeRank, MSG_FIND_NODE, req);
    }

    // Right child
    nextIndex = job.getRightChildIndex();
    if (job.hasRightChild()) {
        // Propagate right
        MyMpi::isend(MPI_COMM_WORLD, job.getRightChildNodeRank(), MSG_UPDATE_VOLUME, payload);
        if (nextIndex >= volume) {
            // Prune child
            Console::log_send(Console::VERB, "Pruning right child of " + job.toStr(), job.getRightChildNodeRank());
            job.unsetRightChild();
        }
    } else if (nextIndex < volume) {
        // Grow right
        JobRequest req(jobId, job.getRootNodeRank(), worldRank, nextIndex, epochCounter.getEpoch(), 0);
        int nextNodeRank = job.getRightChildNodeRank();
        MyMpi::isend(MPI_COMM_WORLD, nextNodeRank, MSG_FIND_NODE, req);
    }

    // Shrink (and pause solving) if necessary
    if (thisIndex > 0 && thisIndex >= volume) {
        jobs[jobId]->suspend();
        load = 0;
    }
}

bool Worker::isTimeForRebalancing() {
    return epochCounter.getSecondsSinceLastSync() >= params.getFloatParam("p");
}

float Worker::allReduce(float contribution) const {
    float result;
    MPI_Allreduce(&contribution, &result, 1, MPI_FLOAT, MPI_SUM, comm);
    return result;
}

float Worker::reduce(float contribution, int rootRank) const {
    float result;
    MPI_Reduce(&contribution, &result, 1, MPI_FLOAT, MPI_SUM, rootRank, comm);
    return result;
}
