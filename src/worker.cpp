
#include <math.h>
#include <thread>
#include <unistd.h>
#include <iostream>
#include <fstream>

#include "worker.h"
#include "util/timer.h"
#include "util/console.h"
#include "util/random.h"
#include "balancing/thermodynamic_balancer.h"
#include "balancing/simple_priority_balancer.h"

void Worker::init() {

    // Initialize synchronized rebalancing clock
    lastRebalancing = Timer::elapsedSeconds();
    exchangedClausesThisRound = false;

    // Initialize balancer
    //balancer = std::unique_ptr<Balancer>(new ThermodynamicBalancer(comm, params));
    balancer = std::unique_ptr<Balancer>(new SimplePriorityBalancer(comm, params));
    
    // Begin listening to an incoming message
    MyMpi::irecv(MPI_COMM_WORLD);
}

void Worker::checkTerminate() {

    bool exitNow = false;
    std::fstream file;
    file.open("TERMINATE_GLOBALLY_NOW", std::ios::in);
    if (file.is_open()) {
        file.close();
        Console::log("Acknowledged TERMINATE_GLOBALLY_NOW. Exiting.");
        exit(0);
    }
}

void Worker::mainProgram() {

    Console::log("Worker node set up.");

    while (true) {

        // Solve loops for each active HordeLib instance
        for (auto it = jobs.begin(); it != jobs.end(); ++it) {
            int jobId = it->first;
            JobImage &job = *it->second;
            if (job.getState() != JobState::ACTIVE)
                continue;
            int result = job.solveLoop();
            if (result >= 0) {
                // Solver done!
                int jobRootRank = job.getRootNodeRank();
                Console::log_send("Found result " + std::string(result == 10 ? "SAT" : result == 20 ? "UNSAT" : "UNKNOWN") + " on " + job.toStr(), jobRootRank);
                std::vector<int> payload;
                payload.push_back(jobId); payload.push_back(result);
                MyMpi::isend(MPI_COMM_WORLD, jobRootRank, MSG_WORKER_FOUND_RESULT, payload);
            }
        }

        if (isTimeForClauseSharing()) {

            // Clause sharing
            for (auto it = jobs.begin(); it != jobs.end(); ++it) {
                int jobId = it->first;
                JobImage& img = *it->second;
                if ((img.getState() == JobState::ACTIVE) 
                    && !img.hasLeftChild() && !img.hasRightChild()) {
                    beginClauseGathering(jobId);
                }
            }
            exchangedClausesThisRound = true;
        }

        if (isTimeForRebalancing()) {

            // Rebalancing
            Console::log("Entering rebalancing");
            iteration++;
            int numOccupiedNodes = allReduce(isIdle() ? 0.0f : 1.0f);
            if (MyMpi::rank(comm) == 0) {
                Console::log("Before rebalancing: " + std::to_string(numOccupiedNodes) + " occupied nodes");
            }
            rebalance();
            exchangedClausesThisRound = false;
        }

        // Poll messages, if present
        MessageHandlePtr handle;
        while ((handle = MyMpi::poll()) != NULL) {
            // Process message

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

            else if (handle->tag == MSG_UPDATE_DEMAND)
                handleUpdateDemand(handle);

            else if (handle->tag == MSG_GATHER_CLAUSES)
                handleGatherClauses(handle);

            else if (handle->tag == MSG_DISTRIBUTE_CLAUSES)
                handleDistributeClauses(handle);

            else if (handle->tag == MSG_WORKER_FOUND_RESULT
                     || handle->tag == MSG_TERMINATE)
                handleTerminate(handle);

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

        checkTerminate();

        // TODO Sleep for a bit
        usleep(1000); // 1000 = 1 millisecond
    }
}

void Worker::handleIntroduceJob(MessageHandlePtr& handle) {

    // Receive job signature and then the actual job
    JobSignature sig; sig.deserialize(handle->recvData);
    MessageHandlePtr jobHandle = MyMpi::recv(MPI_COMM_WORLD, MSG_SEND_JOB, sig.getTransferSize()); // BLOCKING
    Job job; job.deserialize(jobHandle->recvData);

    assert(!hasJobImage(job.getId()));
    JobImage *img = new JobImage(params, MyMpi::size(comm), worldRank, job.getId());
    jobs[job.getId()] = img;
    img->store(job);

    if (isIdle() && !hasJobCommitments()) {
        // Accept and initialize the job
        Console::log_recv("Job #" + std::to_string(job.getId()) + " introduced. Beginning to compute as root node", jobHandle->source);
        img->initialize(/*index=*/0, /*rootRank=*/worldRank, /*parentRank=*/handle->source);
        load = 1;

    } else {
        // Trigger a node finding procedure
        Console::log_recv("Job #" + std::to_string(job.getId()) + " introduced. Bouncing ...", handle->source);
        JobRequest request(job.getId(), worldRank, worldRank, 0, iteration, -1);
        bounceJobRequest(request);
    }
}

void Worker::handleFindNode(MessageHandlePtr& handle) {

    JobRequest req; req.deserialize(handle->recvData);

    if (req.iteration != iteration) {
        Console::log_recv("Discarding a job request from a previous iteration", handle->source);
        return;
    }

    if (hasJobImage(req.jobId) && jobs[req.jobId]->getState() == JobState::PAST) {
        // This job already finished!
        Console::log("Consuming request " + jobStr(req.jobId, req.requestedNodeIndex)
                   + " as it already finished");
        return;
    }

    if (isIdle() && !hasJobCommitments()) {

        Console::log_recv("Willing to adopt " + jobStr(req.jobId, req.requestedNodeIndex)
                        + " after " + std::to_string(req.numHops) + " bounces", handle->source);

        // Commit on the job, send a request to the parent
        bool fullTransfer = false;
        if (!hasJobImage(req.jobId)) {
            // Job is not known yet
            jobs[req.jobId] = new JobImage(params, MyMpi::size(comm), worldRank, req.jobId);
            fullTransfer = true;
        }
        req.fullTransfer = fullTransfer ? 1 : 0;
        jobs[req.jobId]->commit(req);
        jobCommitments[req.jobId] = req;
        MyMpi::isend(MPI_COMM_WORLD, req.requestingNodeRank, MSG_REQUEST_BECOME_CHILD, jobCommitments[req.jobId]);

    } else {
        // Continue job finding procedure
        Console::log_recv("Bouncing " + jobStr(req.jobId, req.requestedNodeIndex), handle->source);
        bounceJobRequest(req);
    }
}

void Worker::handleRequestBecomeChild(MessageHandlePtr& handle) {

    Console::log_recv("Request to become parent", handle->source);
    JobRequest req; req.deserialize(handle->recvData);

    // Retrieve concerned job
    assert(hasJobImage(req.jobId));
    JobImage &img = getJobImage(req.jobId);

    bool reject = false;
    if (req.iteration != iteration) {

        Console::log_send("Request " + img.toStr() + " is from a previous iteration -- rejecting", handle->source);
        reject = true;

    } else if (img.getState() != JobState::ACTIVE 
            && img.getState() != JobState::STORED) {

        Console::log_send(img.toStr() + " is not active and not stored (any more) -- rejecting", handle->source);
        Console::log("My job state: " + img.jobStateToStr());
        reject = true;

    } else {

        const Job& job = getJobImage(req.jobId).getJob();

        // Send job signature
        JobSignature sig(req.jobId, req.rootRank, job.getFormulaSize(), job.getAssumptionsSize());
        MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_ACCEPT_BECOME_CHILD, sig);

        // If req.fullTransfer, then wait for the child to acknowledge having received the signature
        // Else:
        if (req.fullTransfer == 1) {
            Console::log_send("Sending " + jobStr(req.jobId, req.requestedNodeIndex), handle->source);
        } else {
            Console::log_send("Resuming child " + jobStr(req.jobId, req.requestedNodeIndex), handle->source);

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
    assert(hasJobImage(req.jobId));
    JobImage &img = getJobImage(req.jobId);
    assert(img.getState() == JobState::COMMITTED);

    Console::log_recv("Cancelling commitment to " + img.toStr(), handle->source);
    jobCommitments.erase(req.jobId);
    img.uncommit(req);
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
        assert(hasJobImage(req.jobId));
        JobImage& img = getJobImage(req.jobId);
        if (img.getState() == JobState::PAST) {
            Console::log("WARN: " + img.toStr() + " already finished, so it will not be re-initialized");
        } else {
            Console::log_recv("Starting or resuming " + jobStr(req.jobId, req.requestedNodeIndex), handle->source);
            img.reinitialize(req.requestedNodeIndex, req.rootRank, req.requestingNodeRank);
            load = 1;
        }
    }

    jobCommitments.erase(sig.jobId);
}

void Worker::handleAckAcceptBecomeChild(MessageHandlePtr& handle) {
    JobRequest req; req.deserialize(handle->recvData);

    // Retrieve and send concerned job
    assert(hasJobImage(req.jobId));
    JobImage& img = getJobImage(req.jobId);
    const Job &job = img.getJob();
    MyMpi::send(MPI_COMM_WORLD, handle->source, MSG_SEND_JOB, job);

    if (img.getState() == JobState::PAST) {
        // Job already terminated -- send termination signal
        MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_TERMINATE, req.jobId);
    } else {

        // Mark new node as one of the node's children, if applicable
        if (img.isInitialized()) {
            if (req.requestedNodeIndex == img.getLeftChildIndex()) {
                img.setLeftChild(handle->source);
            } else if (req.requestedNodeIndex == img.getRightChildIndex()) {
                img.setRightChild(handle->source);
            } else assert(req.requestedNodeIndex == 0);
        }

        // Send current volume / initial demand update
        if (img.getState() == JobState::ACTIVE) {
            int volume = balancer->getVolume(req.jobId);
            assert(volume >= 1);
            Console::log_send("Propagating volume " + std::to_string(volume) + " to new child", handle->source);
            std::vector<int> jobIdAndDemand;
            jobIdAndDemand.push_back(req.jobId);
            jobIdAndDemand.push_back(volume);
            MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_UPDATE_DEMAND, jobIdAndDemand);
        }
    }
}

void Worker::handleSendJob(MessageHandlePtr& handle) {
    Job job; job.deserialize(handle->recvData);
    //Console::log("Received " + std::to_string(job.getId()));
    jobs[job.getId()]->store(job);
    jobs[job.getId()]->initialize();
    load = 1;
}

void Worker::handleUpdateDemand(MessageHandlePtr& handle) {
    int jobId = handle->recvData[0];
    int demand = handle->recvData[1];
    updateDemand(jobId, demand);
}

void Worker::handleGatherClauses(MessageHandlePtr& handle) {
    collectAndGatherClauses(handle->recvData);
}

void Worker::handleDistributeClauses(MessageHandlePtr& handle) {
    learnAndDistributeClausesDownwards(handle->recvData);
}

void Worker::handleTerminate(MessageHandlePtr& handle) {

    int jobId = handle->recvData[0];
    JobImage& img = getJobImage(jobId);

    // Either this job is still running, or it has been shut down by a previous termination message / rebalancing procedure
    assert(img.getState() == JobState::ACTIVE
        || img.getState() == JobState::PAST
        || img.getState() == JobState::SUSPENDED);

    if (img.getState() == JobState::ACTIVE) {

        // Propagate termination message down the job tree
        if (img.hasLeftChild())
            MyMpi::isend(MPI_COMM_WORLD, img.getLeftChildNodeRank(), MSG_TERMINATE, handle->recvData);
        if (img.hasRightChild())
            MyMpi::isend(MPI_COMM_WORLD, img.getRightChildNodeRank(), MSG_TERMINATE, handle->recvData);

        // Terminate
        Console::log("Terminating " + img.toStr());
        img.withdraw();
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
    int randomOtherNodeRank = getRandomWorkerNode();
    MyMpi::isend(MPI_COMM_WORLD, randomOtherNodeRank, MSG_FIND_NODE, request);
}

void Worker::beginClauseGathering(int jobId) {

    JobImage& img = getJobImage(jobId);
    if (img.getState() != JobState::ACTIVE)
        return;

    std::vector<int> clauses = img.collectClausesFromSolvers();
    if (img.isRoot()) {
        // There are no other nodes computing on this job
        //Console::log("Self-broadcasting clauses");
        img.learnClausesFromAbove(clauses);
        return;
    }

    clauses.push_back(jobId);
    int parentRank = img.getParentNodeRank();
    Console::log_send("Sending clause vector of effective size " + std::to_string(clauses.size()-1)
                    + " from " + img.toStr(), parentRank);
    MyMpi::isend(MPI_COMM_WORLD, parentRank, MSG_GATHER_CLAUSES, clauses);
}

void Worker::collectAndGatherClauses(std::vector<int>& clausesFromAChild) {

    int jobId = clausesFromAChild[clausesFromAChild.size()-1];
    clausesFromAChild.resize(clausesFromAChild.size() - 1);

    Console::log("Received clauses from below of effective size " + std::to_string(clausesFromAChild.size())
                    + " about #" + std::to_string(jobId));

    if (!hasJobImage(jobId)) {
        Console::log("WARN: I don't know that job.");
        return;
    }
    JobImage& img = getJobImage(jobId);
    if (img.getState() != JobState::ACTIVE)
        return;

    img.collectClausesFromBelow(clausesFromAChild);

    if (img.canShareCollectedClauses()) {
        std::vector<int> clausesToShare = img.shareCollectedClauses();
        clausesToShare.push_back(jobId);
        if (img.isRoot()) {
            Console::log("Switching clause exchange from gather to broadcast");
            learnAndDistributeClausesDownwards(clausesToShare);
        } else {
            int parentRank = img.getParentNodeRank();
            Console::log_send("Gathering clauses about " + img.toStr(), parentRank);
            MyMpi::isend(MPI_COMM_WORLD, parentRank, MSG_GATHER_CLAUSES, clausesToShare);
        }
    }
}

void Worker::learnAndDistributeClausesDownwards(std::vector<int>& clauses) {

    int jobId = clauses[clauses.size()-1];
    clauses.resize(clauses.size() - 1);
    Console::log(std::to_string(clauses.size()) + " clauses");
    assert(clauses.size() % BROADCAST_CLAUSE_INTS_PER_NODE == 0);

    if (!hasJobImage(jobId)) {
        Console::log("WARN: Received clauses from above about #" + std::to_string(jobId) + ", but I don't know it.");
        return;
    }
    JobImage& img = getJobImage(jobId);
    if (img.getState() != JobState::ACTIVE)
        return;

    img.learnClausesFromAbove(clauses);

    clauses.push_back(jobId);
    int childRank;
    if (img.hasLeftChild()) {
        childRank = img.getLeftChildNodeRank();
        Console::log_send("Broadcasting clauses about " + img.toStr(), childRank);
        MyMpi::isend(MPI_COMM_WORLD, childRank, MSG_DISTRIBUTE_CLAUSES, clauses);
    }
    if (img.hasRightChild()) {
        childRank = img.getRightChildNodeRank();
        MyMpi::isend(MPI_COMM_WORLD, childRank, MSG_DISTRIBUTE_CLAUSES, clauses);
    }
}

void Worker::rebalance() {

    if (MyMpi::rank(comm) == 0)
        Console::log("Rebalancing ...");

    std::map<int, int> volumes = balancer->balance(jobs);
    for (auto it = volumes.begin(); it != volumes.end(); ++it) {
        updateDemand(it->first, it->second);
    }

    // All collective operations are done; reset synchronized timer
    lastRebalancing = Timer::elapsedSeconds();
}

void Worker::updateDemand(int jobId, int demand) {

    if (!hasJobImage(jobId)) {
        Console::log("WARN: Received a volume update about #"
                + std::to_string(jobId) + ", which is unknown to me.");
        return;
    }

    JobImage &img = getJobImage(jobId);

    if (img.getState() != JobState::ACTIVE) {
        // Job is not active right now
        return;
    }

    // Prepare demand update to propagate down the job tree
    std::vector<int> payload;
    payload.push_back(jobId);
    payload.push_back(demand);

    // Root node update message
    int thisIndex = img.getIndex();
    if (thisIndex == 0) {
        Console::log("Updating demand of #" + std::to_string(jobId) + " to " + std::to_string(demand));
    }

    // Left child
    int nextIndex = img.getLeftChildIndex();
    if (img.hasLeftChild()) {
        // Propagate left
        MyMpi::isend(MPI_COMM_WORLD, img.getLeftChildNodeRank(), MSG_UPDATE_DEMAND, payload);
        if (nextIndex >= demand) {
            // Prune child
            Console::log_send("Pruning left child of " + img.toStr(), img.getLeftChildNodeRank());
            img.unsetLeftChild();
        }
    } else if (nextIndex < demand) {
        // Grow left
        JobRequest req(jobId, img.getRootNodeRank(), worldRank, nextIndex, iteration, 0);
        int nextNodeRank = img.getLeftChildNodeRank();
        MyMpi::isend(MPI_COMM_WORLD, nextNodeRank, MSG_FIND_NODE, req);
    }

    // Right child
    nextIndex = img.getRightChildIndex();
    if (img.hasRightChild()) {
        // Propagate right
        MyMpi::isend(MPI_COMM_WORLD, img.getRightChildNodeRank(), MSG_UPDATE_DEMAND, payload);
        if (nextIndex >= demand) {
            // Prune child
            Console::log_send("Pruning right child of " + img.toStr(), img.getRightChildNodeRank());
            img.unsetRightChild();
        }
    } else if (nextIndex < demand) {
        // Grow right
        JobRequest req(jobId, img.getRootNodeRank(), worldRank, nextIndex, iteration, 0);
        int nextNodeRank = img.getRightChildNodeRank();
        MyMpi::isend(MPI_COMM_WORLD, nextNodeRank, MSG_FIND_NODE, req);
    }
    // TODO Propagate MSG_UPDATE_DEMAND to grown children as soon as they are established (growing by more than one layer)

    // Shrink (and pause solving) if necessary
    if (thisIndex > 0 && thisIndex >= demand) {
        jobs[jobId]->suspend();
        load = 0;
    }
}

bool Worker::isTimeForRebalancing() {
    return Timer::elapsedSeconds() - lastRebalancing >= params.getFloatParam("p");
}

bool Worker::isTimeForClauseSharing() {
    return !exchangedClausesThisRound && Timer::elapsedSeconds() - lastRebalancing >= 2.5f;
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
