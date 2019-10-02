
#include <math.h>
#include <thread>

#include "worker.h"
#include "mpi.h"
#include "random.h"

void Worker::init() {

    // Initialize synchronized rebalancing clock
    lastRebalancing = elapsed_time();
    exchangedClausesThisRound = false;

    // Begin listening to an incoming message
    MyMpi::irecv(MPI_COMM_WORLD);
}

void Worker::mainProgram() {

    MyMpi::log("Worker node set up.");

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
                MyMpi::log_send("Found result " + std::string(result == 10 ? "SAT" : result == 20 ? "UNSAT" : "UNKNOWN") + " on " + job.toStr(), jobRootRank);
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
                if (img.getState() == JobState::ACTIVE && !img.hasLeftChild() && !img.hasRightChild()) {
                    beginClauseGathering(jobId);
                }
            }
            exchangedClausesThisRound = true;
        }

        if (isTimeForRebalancing()) {

            // Rebalancing
            iteration++;
            int numOccupiedNodes = allReduce(isIdle() ? 0.0f : 1.0f);
            if (MyMpi::rank(comm) == 0) {
                MyMpi::log("Before rebalancing: " + std::to_string(numOccupiedNodes) + " occupied nodes");
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

        // TODO Sleep for a bit
    }
}

void Worker::handleIntroduceJob(MessageHandlePtr& handle) {

    // Receive job signature and then the actual job
    JobSignature sig; sig.deserialize(handle->recvData);
    MessageHandlePtr jobHandle = MyMpi::recv(MPI_COMM_WORLD, MSG_SEND_JOB, sig.getTransferSize()); // BLOCKING
    Job job; job.deserialize(jobHandle->recvData);

    assert(!hasJobImage(job.getId()));
    JobImage *img = new JobImage(MyMpi::size(comm), worldRank, job.getId());
    jobs[job.getId()] = img;
    img->store(job);

    if (isIdle() && !hasJobCommitments()) {
        // Accept and initialize the job
        MyMpi::log_recv("Job #" + std::to_string(job.getId()) + " introduced. Beginning to compute as root node", jobHandle->source);
        img->initialize(/*index=*/0, /*rootRank=*/worldRank, /*parentRank=*/handle->source);
        load = 1;

    } else {
        // Trigger a node finding procedure
        MyMpi::log_recv("Job #" + std::to_string(job.getId()) + " introduced. Bouncing ...", handle->source);
        JobRequest request(job.getId(), worldRank, worldRank, 0, iteration, -1);
        bounceJobRequest(request);
    }
}

void Worker::handleFindNode(MessageHandlePtr& handle) {

    JobRequest req; req.deserialize(handle->recvData);

    if (req.iteration != iteration) {
        MyMpi::log_recv("Discarding a job request from a previous iteration", handle->source);
        return;
    }

    if (hasJobImage(req.jobId) && jobs[req.jobId]->getState() == JobState::PAST) {
        // This job already finished!
        MyMpi::log("Consuming request " + jobStr(req.jobId, req.requestedNodeIndex)
                   + " as it already finished");
        return;
    }

    if (isIdle() && !hasJobCommitments()) {

        MyMpi::log_recv("Willing to adopt " + jobStr(req.jobId, req.requestedNodeIndex)
                        + " after " + std::to_string(req.numHops) + " bounces", handle->source);

        // Commit on the job, send a request to the parent
        bool fullTransfer = false;
        if (!hasJobImage(req.jobId)) {
            // Job is not known yet
            jobs[req.jobId] = new JobImage(MyMpi::size(comm), worldRank, req.jobId);
            fullTransfer = true;
        }
        req.fullTransfer = fullTransfer ? 1 : 0;
        jobs[req.jobId]->commit(req);
        jobCommitments[req.jobId] = req;
        MyMpi::isend(MPI_COMM_WORLD, req.requestingNodeRank, MSG_REQUEST_BECOME_CHILD, jobCommitments[req.jobId]);

    } else {
        // Continue job finding procedure
        MyMpi::log_recv("Bouncing " + jobStr(req.jobId, req.requestedNodeIndex), handle->source);
        bounceJobRequest(req);
    }
}

void Worker::handleRequestBecomeChild(MessageHandlePtr& handle) {

    MyMpi::log_recv("Request to become parent", handle->source);
    JobRequest req; req.deserialize(handle->recvData);

    // Retrieve concerned job
    assert(hasJobImage(req.jobId));
    JobImage &img = getJobImage(req.jobId);

    bool reject = false;
    if (req.iteration != iteration) {

        MyMpi::log_send("Request " + img.toStr() + " is from a previous iteration -- rejecting", handle->source);
        reject = true;

    } else if (img.getState() != JobState::ACTIVE && img.getState() != JobState::STORED) {

        MyMpi::log_send(img.toStr() + " is not active and not stored (any more) -- rejecting", handle->source);
        MyMpi::log("My job state: " + img.jobStateToStr());
        reject = true;

    } else {

        const Job& job = getJobImage(req.jobId).getJob();

        // Send job signature
        JobSignature sig(req.jobId, req.rootRank, job.getFormulaSize(), job.getAssumptionsSize());
        MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_ACCEPT_BECOME_CHILD, sig);

        // If req.fullTransfer, then wait for the child to acknowledge having received the signature
        // Else:
        if (req.fullTransfer == 1) {
            MyMpi::log_send("Sending " + jobStr(req.jobId, req.requestedNodeIndex), handle->source);
        } else {
            MyMpi::log_send("Resuming child " + jobStr(req.jobId, req.requestedNodeIndex), handle->source);

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

    MyMpi::log_recv("Cancelling commitment to " + img.toStr(), handle->source);
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
            MyMpi::log("WARN: " + img.toStr() + " already finished, so it will not be re-initialized");
        } else {
            MyMpi::log_recv("Starting or resuming " + jobStr(req.jobId, req.requestedNodeIndex), handle->source);
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
        if (req.requestedNodeIndex == jobs[req.jobId]->getLeftChildIndex()) {
            jobs[req.jobId]->setLeftChild(handle->source);
        } else if (req.requestedNodeIndex == jobs[req.jobId]->getRightChildIndex()) {
            jobs[req.jobId]->setRightChild(handle->source);
        } else assert(req.requestedNodeIndex == 0);

        // Send current volume / initial demand update
        if (img.getState() == JobState::ACTIVE) {
            assert(img.getJob().getVolume() >= 1);
            std::vector<int> jobIdAndDemand;
            jobIdAndDemand.push_back(req.jobId);
            jobIdAndDemand.push_back(img.getJob().getVolume());
            MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_UPDATE_DEMAND, jobIdAndDemand);
        }
    }
}

void Worker::handleSendJob(MessageHandlePtr& handle) {
    Job job; job.deserialize(handle->recvData);
    //MyMpi::log("Received " + std::to_string(job.getId()));
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

    // Either this job is still running, or it has been shut down by a previous termination message / rebalancing procedure
    assert(jobs[jobId]->getState() == JobState::ACTIVE
        || jobs[jobId]->getState() == JobState::PAST
        || jobs[jobId]->getState() == JobState::SUSPENDED);

    if (jobs[jobId]->getState() == JobState::ACTIVE) {

        // Propagate termination message down the job tree
        if (jobs[jobId]->hasLeftChild())
            MyMpi::isend(MPI_COMM_WORLD, jobs[jobId]->getLeftChildNodeRank(), MSG_TERMINATE, handle->recvData);
        if (jobs[jobId]->hasRightChild())
            MyMpi::isend(MPI_COMM_WORLD, jobs[jobId]->getRightChildNodeRank(), MSG_TERMINATE, handle->recvData);

        // Terminate
        MyMpi::log("Terminating " + jobs[jobId]->toStr());
        jobs[jobId]->withdraw();
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
        MyMpi::log("Not self-broadcasting clauses");
        //img.learnClausesFromAbove(clauses);
        return;
    }

    clauses.push_back(jobId);
    int parentRank = img.getParentNodeRank();
    MyMpi::log_send("Sending clause vector of effective size " + std::to_string(clauses.size()-1)
                    + " from " + img.toStr(), parentRank);
    MyMpi::isend(MPI_COMM_WORLD, parentRank, MSG_GATHER_CLAUSES, clauses);
}

void Worker::collectAndGatherClauses(std::vector<int>& clausesFromAChild) {

    int jobId = clausesFromAChild[clausesFromAChild.size()-1];
    clausesFromAChild.resize(clausesFromAChild.size() - 1);

    MyMpi::log("Received clauses from below of effective size " + std::to_string(clausesFromAChild.size())
                    + " about #" + std::to_string(jobId));

    if (!hasJobImage(jobId)) {
        MyMpi::log("WARN: I don't know that job.");
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
            MyMpi::log("Switching clause exchange from gather to broadcast");
            learnAndDistributeClausesDownwards(clausesToShare);
        } else {
            int parentRank = img.getParentNodeRank();
            MyMpi::log_send("Gathering clauses about " + img.toStr(), parentRank);
            MyMpi::isend(MPI_COMM_WORLD, parentRank, MSG_GATHER_CLAUSES, clausesToShare);
        }
    }
}

void Worker::learnAndDistributeClausesDownwards(std::vector<int>& clauses) {

    int jobId = clauses[clauses.size()-1];
    clauses.resize(clauses.size() - 1);
    MyMpi::log(std::to_string(clauses.size()) + " clauses");
    assert(clauses.size() % BROADCAST_CLAUSE_INTS_PER_NODE == 0);

    if (!hasJobImage(jobId)) {
        // TODO ERROR 2: For some reason, this happens
        MyMpi::log("WARN: Received clauses from above about #" + std::to_string(jobId) + ", but I don't know it.");
        return;
    }
    JobImage& img = getJobImage(jobId);
    if (img.getState() != JobState::ACTIVE)
        return;

    // TODO ERROR 1: This probably leads to segfault
    img.learnClausesFromAbove(clauses);

    clauses.push_back(jobId);
    int childRank;
    if (img.hasLeftChild()) {
        childRank = img.getLeftChildNodeRank();
        MyMpi::log_send("Broadcasting clauses about " + img.toStr(), childRank);
        MyMpi::isend(MPI_COMM_WORLD, childRank, MSG_DISTRIBUTE_CLAUSES, clauses);
    }
    if (img.hasRightChild()) {
        childRank = img.getRightChildNodeRank();
        MyMpi::isend(MPI_COMM_WORLD, childRank, MSG_DISTRIBUTE_CLAUSES, clauses);
    }
}

void Worker::rebalance() {

    if (MyMpi::rank(comm) == 0)
        MyMpi::log("Rebalancing ...");

    std::vector<Job> activeJobs;
    std::vector<Job> involvedJobs;
    std::map<int, float> demands;
    for (auto it = jobs.begin(); it != jobs.end(); ++it) {
        JobImage &img = *it->second;
        if ((img.getState() == JobState::ACTIVE) && img.isRoot()) {
            //MyMpi::log("Participating with " + img.toStr() + ", ID " + std::to_string(img.getJob()->getId()));
            const Job& job = img.getJob();

            assert(job.getTemperature() > 0);
            assert(job.getPriority() > 0);

            activeJobs.push_back(job);
            involvedJobs.push_back(job);
            demands[job.getId()] = 0;
        }
    }

    int fullVolume = MyMpi::size(comm);

    // Initial pressure, filtering out micro-jobs
    float remainingVolume = fullVolume * loadFactor;
    assert(remainingVolume > 0);
    float pressure = calculatePressure(involvedJobs, remainingVolume);
    assert(pressure >= 0);
    float microJobDiscount = 0;
    for (unsigned int i = 0; i < involvedJobs.size(); i++) {
        const Job& job = involvedJobs[i];
        float demand = 1/pressure * job.getTemperature() * job.getPriority();
        if (demand < 1) {
            // Micro job!
            microJobDiscount++;
            demands[job.getId()] = 1;
            involvedJobs.erase(involvedJobs.begin() + i);
            i--;
        }
    }
    remainingVolume -= allReduce(microJobDiscount);
    assert(remainingVolume >= 0);

    // Main iterations for computing exact job demands
    int iteration = 0;
    while (remainingVolume >= 1 && iteration <= 10) {
        float unusedVolume = 0;
        pressure = calculatePressure(involvedJobs, remainingVolume);
        if (pressure == 0) break;
        //if (MyMpi::rank(comm) == 0) MyMpi::log("Pressure: " + std::to_string(pressure));
        for (unsigned int i = 0; i < involvedJobs.size(); i++) {
            Job *job = &involvedJobs[i];
            float addition = 1/pressure * job->getTemperature() * job->getPriority();
            //MyMpi::log(std::to_string(pressure) + "," + std::to_string(job->getTemperature()) + "," + std::to_string(job->getPriority()));
            float upperBound = (float) std::min((double) fullVolume, (double) loadFactor*fullVolume);
            upperBound = (float) std::min((double) upperBound, (double) 2*job->getVolume()+1);
            float demand = demands[job->getId()];
            if (demand + addition >= upperBound) {
                // Upper bound hit
                unusedVolume += (demand + addition) - upperBound;
                addition = upperBound - demand;
                involvedJobs.erase(involvedJobs.begin() + i);
                i--;
            }
            assert(addition >= 0);
            demands[job->getId()] += addition;
        }
        remainingVolume = allReduce(unusedVolume);
        iteration++;
    }
    if (MyMpi::rank(comm) == 0)
        MyMpi::log("Did " + std::to_string(iteration) + " rebalancing iterations");

    // Weigh remaining volume against shrinkage
    float shrink = 0;
    for (auto it = activeJobs.begin(); it != activeJobs.end(); ++it) {
        Job job = *it;
        assert(demands[job.getId()] >= 0);
        if (job.getVolume() - demands[job.getId()] > 0) {
            shrink += job.getVolume() - demands[job.getId()];
        }
    }
    float shrinkage = allReduce(shrink);
    float shrinkageFactor;
    if (shrinkage == 0) {
        shrinkageFactor = 0;
    } else {
        shrinkageFactor = (shrinkage - remainingVolume) / shrinkage;
    }
    assert(shrinkageFactor >= 0);
    assert(shrinkageFactor <= 1);

    // Round and apply volume updates
    int allDemands = 0;
    for (auto it = activeJobs.begin(); it != activeJobs.end(); ++it) {

        Job job = *it;
        float delta = demands[job.getId()] - job.getVolume();

        if (delta < 0) {
            delta = delta * shrinkageFactor;
        }
        // Probabilistic rounding
        /*
        float random = Random::rand();
        if (random < delta - std::floor(delta)) {
            delta = std::ceil(delta);
        } else {*/
            delta = std::floor(delta);
        //}

        updateDemand(job.getId(), job.getVolume() + delta);
        allDemands += job.getVolume();

        job.coolDown(); // TODO check if actually done
    }
    allDemands = reduce((float) allDemands, 0);

    // All collective operations are done; reset synchronized timer
    lastRebalancing = elapsed_time();

    if (MyMpi::rank(comm) == 0)
        MyMpi::log("Sum of all demands: " + std::to_string(allDemands));
}

void Worker::updateDemand(int jobId, int demand) {

    if (!hasJobImage(jobId)) {
        MyMpi::log("WARN: Received a volume update about #"
                + std::to_string(jobId) + ", which is unknown to me.");
        return;
    }

    JobImage &img = getJobImage(jobId);

    // Update volume
    img.getJob().setVolume(demand);

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
        MyMpi::log("Updating demand of #" + std::to_string(jobId) + " to " + std::to_string(demand));
    }

    // Left child
    int nextIndex = img.getLeftChildIndex();
    if (img.hasLeftChild()) {
        // Propagate left
        MyMpi::isend(MPI_COMM_WORLD, img.getLeftChildNodeRank(), MSG_UPDATE_DEMAND, payload);
        if (nextIndex >= demand) {
            // Prune child
            MyMpi::log_send("Pruning left child " + img.toStr(), img.getLeftChildNodeRank());
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
            MyMpi::log_send("Pruning right child " + img.toStr(), img.getRightChildNodeRank());
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
    return elapsed_time() - lastRebalancing >= params.getFloatParam("p", 5.0f);
}

bool Worker::isTimeForClauseSharing() {
    return !exchangedClausesThisRound && elapsed_time() - lastRebalancing >= 2.5f;
}

float Worker::calculatePressure(const std::vector<Job>& involvedJobs, float volume) const {

    float contribution = 0;
    for (auto it = involvedJobs.begin(); it != involvedJobs.end(); ++it) {
        const Job& job = *it;
        contribution += job.getTemperature() * job.getPriority();
    }
    float allContributions = allReduce(contribution);
    float pressure = allContributions / volume;
    return pressure;
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
