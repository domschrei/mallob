
#include <cmath>
#include <thread>
#include <unistd.h>
#include <iostream>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <sys/syscall.h>
#include <algorithm>
#include <queue>
#include <utility>

#include "worker.h"
#include "app/threaded_sat_job.h"
#include "app/forked_sat_job.h"
#include "util/timer.h"
#include "util/console.h"
#include "util/random.h"
#include "util/memusage.h"
#include "util/sat_reader.h"
#include "util/fork.h"
#include "balancing/cutoff_priority_balancer.h"
#include "balancing/event_driven_balancer.h"
#include "data/job_description.h"

void mpiMonitor(Worker* worker) {
    while (!worker->exiting) {
        double callStart = 0;
        std::string opName = MyMpi::currentCall(&callStart);
        std::string report = "MMPI %i active handles";
        if (callStart < 0.00001 || opName == "") {
            Console::log(Console::VERB, report.c_str(), MyMpi::getNumActiveHandles());
        } else {
            double elapsed = Timer::elapsedSeconds() - callStart;
            report += ", in \"%s\" for %.3fs";
            Console::log(Console::VERB, report.c_str(), MyMpi::getNumActiveHandles(), opName.c_str(), elapsed);
            if (elapsed > 60.0) {
                // Inside some MPI call for a minute
                Console::fail("MPI call takes too long - aborting");
                exit(1);
            }
        }
        usleep(1000 * 1000); // 1s
    }
}

void Worker::init() {

    // Initialize balancer
    //balancer = std::unique_ptr<Balancer>(new ThermodynamicBalancer(comm, params));
    if (params.getParam("bm") == "ed") {
        // Event-driven balancing
        balancer = std::unique_ptr<Balancer>(new EventDrivenBalancer(comm, params));
    } else {
        // Fixed-period balancing
        balancer = std::unique_ptr<Balancer>(new CutoffPriorityBalancer(comm, params));
    }
    
    // Initialize pseudo-random order of nodes
    if (params.isSet("derandomize")) {
        createExpanderGraph();
    }

    // Begin listening to an incoming message
    MyMpi::beginListening(WORKER);

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

    Console::log(Console::VERB, "Global init barrier ...");
    MPI_Barrier(MPI_COMM_WORLD);
    Console::log(Console::VERB, "Passed global init barrier");

    if (params.isSet("mmpi")) mpiMonitorThread = std::thread(mpiMonitor, this);
    else MyMpi::_monitor_off = true;

    // Initiate single instance solving as the "root node"
    if (params.isSet("sinst") && worldRank == 0) {

        std::string instanceFilename = params.getParam("sinst");
        Console::log(Console::INFO, "Initiate solving of single instance \"%s\"", instanceFilename.c_str());

        // Create job description with formula
        Console::log(Console::VERB, "read instance");
        int jobId = 1;
        JobDescription desc(jobId, /*prio=*/1, /*incremental=*/false);
        auto formula = SatReader(instanceFilename).read();
        desc.addPayload(formula);
        desc.addAssumptions(VecPtr(new std::vector<int>()));
        desc.setRootRank(0);

        // Add as a new local SAT job image
        Console::log(Console::VERB, "init SAT job image");
        jobs[jobId] = createJob(params, MyMpi::size(comm), worldRank, jobId, epochCounter);
        JobRequest req(jobId, 0, 0, 0, 0, 0);
        jobs[jobId]->commit(req);
        jobArrivals[jobId] = Timer::elapsedSeconds();
        setLoad(1, jobId);
        jobs[jobId]->setDescription(desc);
        getJob(jobId).beginInitialization();

        // Initialize job in separate thread
        Console::log(Console::VERB, "start init thread");
        initializerThreads[jobId] = std::thread([this, jobId]{
            this->jobs[jobId]->initialize();
        });
    }
}

void Worker::createExpanderGraph() {

    // Pick fixed number k of bounce destinations
    int numBounceAlternatives = params.getIntParam("ba");
    int numWorkers = MyMpi::size(comm);

    // Check validity of num bounce alternatives
    if (2*numBounceAlternatives > numWorkers) {
        numBounceAlternatives = numWorkers / 2;
        Console::log(Console::WARN, "[WARN] Num bounce alternatives must be at most half the number of workers!");
        Console::log(Console::WARN, "[WARN] Falling back to safe value r=%i.", numBounceAlternatives);
    }  

    // Create graph, get outgoing edges from this node
    bounceAlternatives = AdjustablePermutation::createExpanderGraph(numWorkers, numBounceAlternatives, worldRank);
    assert(bounceAlternatives.size() == numBounceAlternatives);

    // Output found bounce alternatives
    std::string info = "";
    for (int i = 0; i < bounceAlternatives.size(); i++) {
        info += std::to_string(bounceAlternatives[i]) + " ";
    }
    Console::log(Console::VERB, "My bounce alternatives: %s", info.c_str());
}

bool Worker::checkTerminate() {
    if (exiting) return true;
    if (globalTimeout > 0 && Timer::elapsedSeconds() > globalTimeout) {
        Console::log(Console::INFO, "Global timeout: terminating.");
        return true;
    }
    return false;
}

void Worker::mainProgram() {

    int iteration = 0;
    float lastMemCheckTime = Timer::elapsedSeconds();
    float lastJobCheckTime = Timer::elapsedSeconds();
    float sleepMicrosecs = params.getFloatParam("sleep");

    float memCheckPeriod = 3.0;
    float jobCheckPeriod = 0.01;

    bool doYield = params.isSet("yield");

    while (!checkTerminate()) {

        float time = Timer::elapsedSeconds();

        if (time - lastMemCheckTime > memCheckPeriod) {
            // Print stats

            // For the this process
            double vm_usage, resident_set; int cpu;
            process_mem_usage(cpu, vm_usage, resident_set);
            vm_usage *= 0.001 * 0.001;
            resident_set *= 0.001 * 0.001;
            Console::log(Console::VERB, "mem cpu=%i vm=%.4fGB rss=%.4fGB", cpu, vm_usage, resident_set);
            myState[2] = resident_set;

            // For this "management" thread
            double perc_cpu;
            bool success = thread_cpuratio(syscall(__NR_gettid), time, perc_cpu);
            if (success) {
                Console::log(Console::VERB, "main : %.2f%% CPU", perc_cpu);
            }

            // For the current job
            if (currentJob != NULL) currentJob->appl_dumpStats();

            lastMemCheckTime = time;

            // Forget jobs that are old or wasting memory
            forgetOldJobs();
        }

        // If it is time to do balancing (and it is not being done right now)
        if (!balancer->isBalancing() && isTimeForRebalancing()) {
            // Rebalancing
            rebalance();

        } else if (balancer->isBalancing()) {

            // Advance balancing if possible (e.g. an iallreduce finished)
            if (balancer->canContinueBalancing()) {
                bool done = balancer->continueBalancing();
                if (done) finishBalancing();
            }
        }

        // Check active HordeLib instance
        //float jobTime = 0;
        if (!isIdle() && time-lastJobCheckTime >= jobCheckPeriod) {
            //jobTime = Timer::elapsedSeconds();
            lastJobCheckTime = time; // jobTime;

            Job &job = *currentJob;
            int id = job.getId();

            bool abort = false;
            if (job.isRoot()) abort = checkComputationLimits(id);
            if (abort) {
                // Timeout (CPUh or wallclock time) hit
                timeoutJob(id);
            } else {
                // Finish initialization as needed
                if (job.isDoneInitializing()) {
                    job.endInitialization();
                    if (job.isRoot()) {
                        // Root worker finished initialization: begin growing if applicable
                        if (balancer->hasVolume(id)) updateVolume(id, balancer->getVolume(id));
                    } else {
                        // Non-root worker finished initialization
                        IntVec payload({job.getId()});
                        MyMpi::isend(MPI_COMM_WORLD, job.getParentNodeRank(), MSG_QUERY_VOLUME, payload);
                    }
                }

                // Check if a result was found
                int result = job.appl_solveLoop();
                if (result >= 0) {
                    // Solver done!
                    job.appl_dumpStats();

                    // Signal termination to root -- may be a self message
                    int jobRootRank = job.getRootNodeRank();
                    IntVec payload({job.getId(), job.getRevision(), result});
                    Console::log_send(Console::VERB, jobRootRank, "%s : sending finished info", job.toStr());
                    MyMpi::isend(MPI_COMM_WORLD, jobRootRank, MSG_WORKER_FOUND_RESULT, payload);
                    job.setResultTransferPending(true);
                    //stats.increment("sentMessages");
                }
            }

            // Job communication (e.g. clause sharing)
            if (job.wantsToCommunicate()) {
                job.communicate();
            }

            //jobTime = Timer::elapsedSeconds() - jobTime;
        }

        // Advance an all-reduction of the current system state
        allreduceSystemState();

        // Poll messages
        //float pollTime = Timer::elapsedSeconds();
        std::vector<MessageHandlePtr> handles = MyMpi::poll();
        /*
        pollTime = Timer::elapsedSeconds() - pollTime;
        if (handles.size() > 0) {
            Console::log(Console::VVVERB, "loop cycle %i", iteration);
            if (jobTime > 0) Console::log(Console::VVVERB, "job time: %.6f s", jobTime);
            Console::log(Console::VVVERB, "poll time: %.6f s", pollTime);
        }*/

        // Process new messages
        for (MessageHandlePtr& handle : handles) {
            Console::log_recv(Console::VVVERB, handle->source, "Process msg id=%i, tag %i", handle->id, handle->tag);
            //float time = Timer::elapsedSeconds();

            if (handle->tag == MSG_FIND_NODE_ONESHOT)
                handleFindNode(handle, /*oneshot=*/true);

            else if (handle->tag == MSG_FIND_NODE)
                handleFindNode(handle, /*oneshot=*/false);

            else if (handle->tag == MSG_ONESHOT_DECLINED)
                handleDeclineOneshot(handle);

            else if (handle->tag == MSG_QUERY_VOLUME)
                handleQueryVolume(handle);
            
            else if (handle->tag == MSG_OFFER_ADOPTION)
                handleOfferAdoption(handle);

            else if (handle->tag == MSG_REJECT_ADOPTION_OFFER)
                handleRejectAdoptionOffer(handle);

            else if (handle->tag == MSG_ACCEPT_ADOPTION_OFFER)
                handleAcceptAdoptionOffer(handle);

            else if (handle->tag == MSG_CONFIRM_ADOPTION)
                handleConfirmAdoption(handle);

            else if (handle->tag == MSG_SEND_JOB_DESCRIPTION)
                handleSendJob(handle);

            else if (handle->tag == MSG_UPDATE_VOLUME)
                handleUpdateVolume(handle);

            else if (handle->tag == MSG_JOB_COMMUNICATION)
                handleJobCommunication(handle);

            else if (handle->tag == MSG_WORKER_FOUND_RESULT)
                handleWorkerFoundResult(handle);
            
            else if (handle->tag == MSG_RESULT_OBSOLETE)
                handleResultObsolete(handle);

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

            else if (handle->tag == MSG_JOB_DONE)
                handleJobDone(handle);

            else if (handle->tag == MSG_SEND_JOB_RESULT)
                handleSendJobResult(handle);

            else if (handle->tag == MSG_EXIT) 
                handleExit(handle);
            
            else if (handle->tag == MSG_COLLECTIVES || 
                    handle->tag == MSG_ANYTIME_REDUCTION || 
                    handle->tag == MSG_ANYTIME_BROADCAST) {
                // "Collectives" messages are currently handled only in balancer
                bool done = balancer->continueBalancing(handle);
                if (done) finishBalancing();

            } else if (handle->tag == MSG_WARMUP) 
                Console::log_recv(Console::VVVERB, handle->source, "Warmup msg");
            else
                Console::log_recv(Console::WARN, handle->source, "[WARN] Unknown message tag %i", handle->tag);

            //time = Timer::elapsedSeconds() - time;
            //Console::log(Console::VVVERB, "Processing msg, tag %i took %.4f s", handle->tag, time);
            //pollTime = Timer::elapsedSeconds();
        }
        
        MyMpi::testSentHandles();

        if (sleepMicrosecs > 0) {
            // Sleep
            usleep(sleepMicrosecs);
        }
        if (doYield) {
            // Yield thread, e.g. for some SAT thread
            std::this_thread::yield();
        }

        iteration++;
    }

    //while (MyMpi::hasOpenSentHandles()) MyMpi::testSentHandles();
    Console::flush();
    fflush(stdout);
}

void Worker::handleQueryVolume(MessageHandlePtr& handle) {

    IntVec payload(*handle->recvData);
    int jobId = payload[0];

    // No volume of this job (yet?) -- ignore.
    if (!hasJob(jobId) || !balancer->hasVolume(jobId)) return;

    int volume = balancer->getVolume(jobId);
    IntVec response({jobId, volume});

    Console::log_send(Console::VERB, handle->source, "Answer #%i volume query with v=%i", jobId, volume);
    MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_UPDATE_VOLUME, response);
}

void Worker::handleFindNode(MessageHandlePtr& handle, bool oneshot) {

    JobRequest req; req.deserialize(*handle->recvData);

    // Discard request if it has become obsolete
    if (isRequestObsolete(req)) {
        Console::log_recv(Console::VERB, handle->source, "Discard req. %s from time %.2f", 
                    jobStr(req.jobId, req.requestedNodeIndex).c_str(), req.timeOfBirth);
        return;
    }

    // Decide whether job should be adopted or bounced to another node
    bool adopts = false;
    
    if (hasJob(req.jobId) && (getJob(req.jobId).isPast() || getJob(req.jobId).isForgetting())) {
        // Can mean that the job finished in the meantime or that
        // it is in the process of being cleaned up.
        Console::log(Console::VERB, "Reject req. %s : PAST/FORGETTING", 
                        jobStr(req.jobId, req.requestedNodeIndex).c_str());

    } else if (isIdle() && !hasJobCommitments()) {
        // Node is idle and not committed to another job: OK

        if (oneshot) {
            adopts = getJob(req.jobId).isSuspended();
        } else adopts = true;

    } else if (req.requestedNodeIndex == 0 
            && req.numHops > 50 //std::max(50, MyMpi::size(comm)/2)
            && !hasJobCommitments()) {
        // Request for a root node exceeded max #hops: 
        // Possibly adopt the job while dismissing the active job

        // Consider adoption only if that job is unknown or inactive 
        if (!hasJob(req.jobId) || !getJob(req.jobId).isActive()) {

            // Look for an active job that can be suspended
            if (currentJob != NULL) {
                Job& job = *currentJob;
                // Job must be active and a non-root leaf node
                if (job.isActive() && !job.isRoot() && job.isLeaf()) {
                    
                    // Inform parent node of the original job  
                    Console::log(Console::VERB, "Suspend %s ...", job.toStr());
                    Console::log(Console::VERB, "... to adopt starving %s", 
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
        Console::log_recv(Console::VERB, handle->source, "Adopting %s after %i hops", jobstr.c_str(), req.numHops);
        assert(isIdle() || Console::fail("Adopting a job, but not idle!"));
        //stats.push_back("hops", req.numHops);

        // Commit on the job, send a request to the parent
        bool fullTransfer = false;
        if (!hasJob(req.jobId)) {
            // Job is not known yet: create instance, request full transfer
            jobs[req.jobId] = createJob(params, MyMpi::size(comm), worldRank, req.jobId, epochCounter);
            fullTransfer = true;
        } else if (!getJob(req.jobId).hasJobDescription() && !initializerThreads.count(req.jobId)) {
            // Job is known, but never received full description, and no initializer thread is preparing it:
            // request full transfer
            fullTransfer = true;
        }
        req.fullTransfer = fullTransfer ? 1 : 0;
        jobs[req.jobId]->commit(req);
        jobCommitments[req.jobId] = req;
        MyMpi::isend(MPI_COMM_WORLD, req.requestingNodeRank, MSG_OFFER_ADOPTION, jobCommitments[req.jobId]);
        //stats.increment("sentMessages");

    } else {
        if (oneshot) {
            Console::log_send(Console::VVVERB, handle->source, "decline oneshot request for %s", jobStr(req.jobId, req.requestedNodeIndex).c_str());
            MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_ONESHOT_DECLINED, handle->recvData);
        } else {
            // Continue job finding procedure somewhere else
            bounceJobRequest(req, handle->source);
        }
    }
}

void Worker::handleDeclineOneshot(MessageHandlePtr& handle) {
    JobRequest req; req.deserialize(*handle->recvData);
    Console::log_recv(Console::VVVERB, handle->source, 
        "%s : dormant child declined", jobStr(req.jobId, req.requestedNodeIndex).c_str());

    if (isAdoptionOfferObsolete(req)) return;

    Job& job = getJob(req.jobId);
    job.addFailToDormantChild(handle->source);

    bool doNormalHopping = false;
    if (req.numHops > std::max(params.getIntParam("jc"), 2)) {
        // Oneshot node finding exceeded
        doNormalHopping = true;
    } else {
        // Attempt another oneshot request
        req.numHops++;
        // Get dormant children without the node that just declined
        std::set<int> dormantChildren = job.getDormantChildren();
        if (dormantChildren.count(handle->source)) dormantChildren.erase(handle->source);
        if (dormantChildren.empty()) {
            // No fitting dormant children left
            doNormalHopping = true;
        } else {
            // Pick a dormant child, forward request
            int rank = Random::choice(dormantChildren);
            MyMpi::isend(MPI_COMM_WORLD, rank, MSG_FIND_NODE_ONESHOT, req);
            Console::log_send(Console::VERB, rank, "%s : query dormant child", job.toStr());
        }
    }

    if (doNormalHopping) {
        Console::log(Console::VERB, "%s : switch to normal hops", job.toStr());
        req.numHops = -1;
        bounceJobRequest(req, handle->source);
    }
}

void Worker::handleOfferAdoption(MessageHandlePtr& handle) {

    JobRequest req; req.deserialize(*handle->recvData);
    Console::log_recv(Console::VERB, handle->source, "Adoption offer for %s", 
                    jobStr(req.jobId, req.requestedNodeIndex).c_str());

    bool reject = false;
    if (!hasJob(req.jobId)) {
        reject = true;

    } else {
        // Retrieve concerned job
        Job &job = getJob(req.jobId);

        // Check if node should be adopted or rejected
        if (isAdoptionOfferObsolete(req)) {
            // Obsolete request
            Console::log_recv(Console::VERB, handle->source, "Reject offer %s from time %.2f", 
                                job.toStr(), req.timeOfBirth);
            reject = true;

        } else {
            // Adopt the job
            // TODO Check if this node already has the full description!
            const JobDescription& desc = job.getDescription();

            // Send job signature
            JobSignature sig(req.jobId, req.rootRank, req.revision, desc.getTransferSize(true));
            MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_ACCEPT_ADOPTION_OFFER, sig);
            //stats.increment("sentMessages");

            // If req.fullTransfer, then wait for the child to acknowledge having received the signature
            if (req.fullTransfer == 1) {
                Console::log_send(Console::VERB, handle->source, "Will send desc. of %s", jobStr(req.jobId, req.requestedNodeIndex).c_str());
            } else {
                Console::log_send(Console::VERB, handle->source, "Resume child %s", jobStr(req.jobId, req.requestedNodeIndex).c_str());
            }
            // Child *will* start / resume its job solvers
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
        MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_REJECT_ADOPTION_OFFER, req);
        //stats.increment("sentMessages");
    }
}

void Worker::handleRejectAdoptionOffer(MessageHandlePtr& handle) {

    // Retrieve committed job
    JobRequest req; req.deserialize(*handle->recvData);
    if (!hasJob(req.jobId)) return;
    Job &job = getJob(req.jobId);
    if (!job.isCommitted()) return; // Commitment was already erased

    // Erase commitment
    Console::log_recv(Console::VERB, handle->source, "Rejected to become %s : uncommitting", job.toStr());
    jobCommitments.erase(req.jobId);
    job.uncommit();
}

void Worker::handleAcceptAdoptionOffer(MessageHandlePtr& handle) {

    // Retrieve according job commitment
    JobSignature sig; sig.deserialize(*handle->recvData);
    if (!jobCommitments.count(sig.jobId)) {
        Console::log(Console::WARN, "[WARN] Job commitment for #%i not present despite adoption accept msg", sig.jobId);
        return;
    }

    JobRequest& req = jobCommitments[sig.jobId];

    if (req.fullTransfer == 1) {
        // Full transfer of job description is required:
        // Send ACK to parent and receive full job description
        Console::log(Console::VERB, "Will receive desc. of #%i, size %i", req.jobId, sig.getTransferSize());
        MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_CONFIRM_ADOPTION, req);
        //stats.increment("sentMessages");
        MyMpi::irecv(MPI_COMM_WORLD, handle->source, MSG_SEND_JOB_DESCRIPTION, sig.getTransferSize()); // to be received later
        //stats.increment("receivedMessages");

    } else {
        // Already has job description: Directly resume job (if not terminated yet)
        assert(hasJob(req.jobId));
        Job& job = getJob(req.jobId);
        if (!job.hasJobDescription() && !job.isInitializing()) {
            Console::log(Console::WARN, "[WARN] %s has no desc. although full transfer was not requested", job.toStr());
        } else if (!job.isPast()) {
            Console::log_recv(Console::INFO, handle->source, "Reactivate %s (state: %s)", 
                        jobStr(req.jobId, req.requestedNodeIndex).c_str(), job.jobStateToStr());
            setLoad(1, req.jobId);
            job.reactivate(req.requestedNodeIndex, req.rootRank, req.requestingNodeRank);
            // Query volume of job
            MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_QUERY_VOLUME, IntVec({req.jobId}));
        }
        // Erase job commitment
        jobCommitments.erase(sig.jobId);
    }
}

void Worker::handleConfirmAdoption(MessageHandlePtr& handle) {
    JobRequest req; req.deserialize(*handle->recvData);

    // If job already terminated, the description contains the job id ONLY
    if (!hasJob(req.jobId)) {
        MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_SEND_JOB_DESCRIPTION, IntVec({req.jobId}));
        return;
    }
    Job& job = getJob(req.jobId);
    if (job.isPast() || job.isForgetting()) {
        MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_SEND_JOB_DESCRIPTION, IntVec({req.jobId}));
        return;
    }

    // Retrieve and send concerned job description
    MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_SEND_JOB_DESCRIPTION, job.getSerializedDescription());
    //stats.increment("sentMessages");
    Console::log_send(Console::VERB, handle->source, "Sent job desc. of %s", job.toStr());

    // Mark new node as one of the node's children
    if (req.requestedNodeIndex == job.getLeftChildIndex()) {
        job.setLeftChild(handle->source);
    } else if (req.requestedNodeIndex == job.getRightChildIndex()) {
        job.setRightChild(handle->source);
    } else assert(req.requestedNodeIndex == 0);

    // Send current volume / initial demand update
    /*
    if (job.isActive()) {
        int volume = balancer->getVolume(req.jobId);
        assert(volume >= 1);
        Console::log_send(Console::VERB, handle->source, "Propagate v=%i to new child", volume);
        MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_UPDATE_VOLUME, IntPair(req.jobId, volume));
        //stats.increment("sentMessages");
    }*/
}

void Worker::handleSendJob(MessageHandlePtr& handle) {
    auto data = handle->recvData;
    Console::log_recv(Console::VVVERB, handle->source, "Receiving some desc. of size %i", data->size());
    int jobId; memcpy(&jobId, data->data(), sizeof(int));

    if (!hasJob(jobId) || getJob(jobId).isPast()) {
        Console::log(Console::WARN, "[WARN] I don't know job #%i : discard desc.", jobId);
        return;
    }

    // Erase job commitment
    if (jobCommitments.count(jobId)) jobCommitments.erase(jobId);

    // Empty job description
    if (handle->recvData->size() == sizeof(int)) {
        Console::log(Console::VERB, "Received empty desc. of #%i - uncommit", jobId);
        if (getJob(jobId).isCommitted()) getJob(jobId).uncommit();
        return;
    }

    // Initialize job inside a separate thread
    setLoad(1, jobId);
    getJob(jobId).beginInitialization();
    Console::log(Console::VERB, "Received desc. of #%i - initializing", jobId);

    assert(!initializerThreads.count(jobId) || Console::fail("%s already has an initializer thread!", getJob(jobId).toStr()));
    initializerThreads[jobId] = std::thread(&Worker::initJob, this, handle);
}

void Worker::initJob(MessageHandlePtr handle) {
    
    // Deserialize job description
    assert(handle->recvData->size() >= sizeof(int));
    int jobId; memcpy(&jobId, handle->recvData->data(), sizeof(int));
    Console::log_recv(Console::VERB, handle->source, "Deserialize job #%i, desc. of size %i", jobId, handle->recvData->size());
    Job& job = getJob(jobId);
    job.setDescription(handle->recvData);

    // Remember arrival and initialize used CPU time (if root node)
    jobArrivals[jobId] = Timer::elapsedSeconds();
        
    if (job.isPast()) {
        // Job was already aborted
        job.terminate();
    } else if (!job.isForgetting()) {
        // Initialize job
        job.initialize();
    }

    Console::log(Console::VVVVERB, "%s : init thread done", job.toStr());
}

void Worker::handleUpdateVolume(MessageHandlePtr& handle) {
    IntPair recv(*handle->recvData);
    int jobId = recv.first;
    int volume = recv.second;
    if (!hasJob(jobId)) {
        Console::log(Console::WARN, "[WARN] Volume update for unknown #%i", jobId);
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
        Console::log(Console::WARN, "[WARN] Job message from unknown job #%i", jobId);
        return;
    }
    // Give message to corresponding job
    Job& job = getJob(jobId);
    if (job.isActive()) job.appl_communicate(handle->source, msg);
}

void Worker::handleWorkerFoundResult(MessageHandlePtr& handle) {

    // Retrieve job
    IntVec res(*handle->recvData);
    int jobId = res[0];
    int revision = res[1];
    if (!hasJob(jobId) || !getJob(jobId).isRoot()) {
        Console::log(Console::WARN, "[WARN] Invalid adressee for job result of #%i", jobId);
        MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_RESULT_OBSOLETE, handle->recvData);
        return;
    }
    if (getJob(jobId).isPast()) {
        Console::log_recv(Console::VERB, handle->source, "Discard obsolete result for job #%i", jobId);
        MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_RESULT_OBSOLETE, handle->recvData);
        return;
    }
    if (getJob(jobId).getRevision() > revision) {
        Console::log_recv(Console::VERB, handle->source, "Discard obsolete result for job #%i rev. %i", jobId, revision);
        MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_RESULT_OBSOLETE, handle->recvData);
        return;
    }
    
    Console::log_recv(Console::VERB, handle->source, "Result found for job #%i", jobId);

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
        Console::log_send(Console::VERB, handle->source, "Forward rank of client (%i)", payload.second); 
    }

    // Terminate job and propagate termination message
    if (getJob(jobId).getDescription().isIncremental()) {
        handleInterrupt(handle);
    } else {
        handleTerminate(handle);
    }
}

void Worker::handleResultObsolete(MessageHandlePtr& handle) {

    IntVec res(*handle->recvData);
    int jobId = res[0];
    //int revision = res[1];
    if (!hasJob(jobId)) return;
    Console::log_recv(Console::VERB, handle->source, "job result for %s unwanted", getJob(jobId).toStr());
    getJob(jobId).setResultTransferPending(false);
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
    Console::log_send(Console::VERB, handle->source, "Send full result to client");
    MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_SEND_JOB_RESULT, result);
    getJob(jobId).setResultTransferPending(false);
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
    if (!hasJob(jobId)) return;

    if (getJob(jobId).isRoot()) {
        // Forward information on aborted job to client
        MyMpi::isend(MPI_COMM_WORLD, getJob(jobId).getParentNodeRank(), MSG_ABORT, handle->recvData);
    }

    interruptJob(handle, jobId, /*terminate=*/true, /*reckless=*/true);
}

void Worker::handleWorkerDefecting(MessageHandlePtr& handle) {

    // Retrieve job
    IntPair recv(*handle->recvData);
    int jobId = recv.first;
    int index = recv.second;
    if (!hasJob(jobId)) return;
    Job& job = getJob(jobId);

    // Which child is defecting?
    if (job.hasLeftChild() && job.getLeftChildIndex() == index && job.getLeftChildNodeRank() == handle->source) {
        // Prune left child
        job.unsetLeftChild();
    } else if (job.hasRightChild() && job.getRightChildIndex() == index && job.getRightChildNodeRank() == handle->source) {
        // Prune right child
        job.unsetRightChild();
    } else {
        Console::log(Console::VERB, "%s : unknown child %s defecting", job.toStr(), jobStr(jobId, index).c_str());
    }

    // If necessary, find replacement
    if (index < job.getLastVolume()) {

        int nextNodeRank = -1;
        int tag = MSG_FIND_NODE;

        // Try to find a dormant child that is not the message source
        std::set<int> dormantChildren = job.getDormantChildren();
        for (int child : dormantChildren) {
            if (child != handle->source) {
                nextNodeRank = child;
                tag = MSG_FIND_NODE_ONESHOT;
                break;
            }
        }
        // Otherwise, pick a random node
        if (nextNodeRank == -1 && params.isSet("derandomize")) {
            nextNodeRank = Random::choice(bounceAlternatives);
        } else if (nextNodeRank == -1) {
            nextNodeRank = getRandomWorkerNode();
        }

        // Initiate search for a replacement for the defected child
        JobRequest req(jobId, job.getRootNodeRank(), worldRank, index, Timer::elapsedSeconds(), 0);
        Console::log_send(Console::VERB, nextNodeRank, "%s : try to find child replacing defected %s", 
                        job.toStr(), jobStr(jobId, index).c_str());
        MyMpi::isend(MPI_COMM_WORLD, nextNodeRank, tag, req);
        //stats.increment("sentMessages");
    }
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
        Console::log(Console::WARN, "[WARN] Useless revision update #%i rev. %i (I am already at rev. %i)", jobId, revision, lastKnownRevision);
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
    Console::log_recv(Console::VERB, handle->source, "Received exit signal");

    // Forward exit signal
    if (worldRank*2+1 < MyMpi::size(MPI_COMM_WORLD))
        MyMpi::isend(MPI_COMM_WORLD, worldRank*2+1, MSG_EXIT, handle->recvData);
    if (worldRank*2+2 < MyMpi::size(MPI_COMM_WORLD))
        MyMpi::isend(MPI_COMM_WORLD, worldRank*2+2, MSG_EXIT, handle->recvData);
    while (MyMpi::hasOpenSentHandles()) MyMpi::testSentHandles();

    exiting = true;
}

void Worker::interruptJob(MessageHandlePtr& handle, int jobId, bool terminate, bool reckless) {

    if (!hasJob(jobId)) return;
    Job& job = getJob(jobId);

    // Propagate message down the job tree
    int msgTag;
    if (terminate && reckless) msgTag = MSG_ABORT;
    else if (terminate) msgTag = MSG_TERMINATE;
    else msgTag = MSG_INTERRUPT;
    if (job.hasLeftChild()) {
        MyMpi::isend(MPI_COMM_WORLD, job.getLeftChildNodeRank(), msgTag, handle->recvData);
        Console::log_send(Console::VERB, job.getLeftChildNodeRank(), "Propagate interruption of %s ...", job.toStr());
        //stats.increment("sentMessages");
    }
    if (job.hasRightChild()) {
        MyMpi::isend(MPI_COMM_WORLD, job.getRightChildNodeRank(), msgTag, handle->recvData);
        Console::log_send(Console::VERB, job.getRightChildNodeRank(), "Propagate interruption of %s ...", job.toStr());
        //stats.increment("sentMessages");
    }
    for (auto childRank : job.getPastChildren()) {
        MyMpi::isend(MPI_COMM_WORLD, childRank, msgTag, handle->recvData);
        Console::log_send(Console::VERB, childRank, "Propagate interruption of %s (past child) ...", job.toStr());
    }
    job.getPastChildren().clear();

    // Stop / terminate
    if (job.isInitializing() || job.isActive() || job.isSuspended()) {
        Console::log(Console::INFO, "%s : interrupt (state: %s)", job.toStr(), job.jobStateToStr());
        job.stop(); // Solvers are interrupted, not suspended!
        Console::log(Console::VERB, "%s : interrupted", job.toStr());
        if (terminate) {
            if (getLoad() && currentJob->getId() == job.getId()) setLoad(0, job.getId());
            job.terminate();
            Console::log(Console::INFO, "%s : terminated", job.toStr());
            balancer->updateVolume(jobId, 0);
        } 
    }
    // Mark committed job as "PAST"
    if (job.isCommitted() && terminate) {
        if (jobCommitments.count(jobId)) jobCommitments.erase(jobId);
        job.uncommit();
        job.terminate();
    }
}

void Worker::informClient(int jobId, int clientRank) {
    const JobResult& result = getJob(jobId).getResult();

    // Send "Job done!" with advertised result size to client
    Console::log_send(Console::VERB, clientRank, "%s : send JOB_DONE to client", getJob(jobId).toStr());
    IntPair payload(jobId, result.getTransferSize());
    MyMpi::isend(MPI_COMM_WORLD, clientRank, MSG_JOB_DONE, payload);
    //stats.increment("sentMessages");
}

void Worker::handleJobDone(MessageHandlePtr& handle) {
    IntPair recv(*handle->recvData);
    int jobId = recv.first;
    int resultSize = recv.second;
    Console::log_recv(Console::VERB, handle->source, "Will receive job result, length %i, for job #%i", resultSize, jobId);
    MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_QUERY_JOB_RESULT, handle->recvData);
    MyMpi::irecv(MPI_COMM_WORLD, handle->source, MSG_SEND_JOB_RESULT, resultSize);
}

void Worker::handleSendJobResult(MessageHandlePtr& handle) {
    JobResult jobResult; jobResult.deserialize(*handle->recvData);
    int jobId = jobResult.id;
    int resultCode = jobResult.result;
    int revision = jobResult.revision;

    Console::log_recv(Console::INFO, handle->source, "Received result of job #%i rev. %i, code: %i", jobId, revision, resultCode);
    Console::log_noprefix(Console::CRIT, "s %s", resultCode == 10 ? "SATISFIABLE" : resultCode == 20 ? "UNSATISFIABLE" : "UNKNOWN");
    if (resultCode == 10) {
        std::string model = "";
        for (int x = 1; x < jobResult.solution.size(); x++) {
            model += std::to_string(jobResult.solution[x]) + " ";
        }
        Console::log_noprefix(Console::CRIT, "v %s", model.c_str());
    }

    if (params.isSet("sinst")) {
        // Single instance solving is done: begin exit signal
        MyMpi::isend(MPI_COMM_WORLD, 0, MSG_EXIT, IntVec({0}));
    }
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
        Console::log(Console::WARN, "[WARN] Hop no. %i for %s", num, jobStr(request.jobId, request.requestedNodeIndex).c_str());
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
    Console::log_send(Console::VVVERB, nextRank, "Hop %s", jobStr(request.jobId, request.requestedNodeIndex).c_str());
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
    epochCounter.resetLastSync();

    // Retrieve balancing results
    Console::log(Console::VVVERB, "Finishing balancing ...");
    jobVolumes = balancer->getBalancingResult();
    Console::log(MyMpi::rank(comm) == 0 ? Console::VERB : Console::VVVERB, "Balancing completed.");

    // Add last slice of idle/busy time 
    //stats.add((load == 1 ? "busyTime" : "idleTime"), Timer::elapsedSeconds() - lastLoadChange);
    lastLoadChange = Timer::elapsedSeconds();

    // Advance to next epoch
    epochCounter.increment();
    Console::log(Console::VVVERB, "Advancing to epoch %i", epochCounter.getEpoch());
    
    // Update volumes found during balancing, and trigger job expansions / shrinkings
    for (auto it = jobVolumes.begin(); it != jobVolumes.end(); ++it) {
        int jobId = it->first;
        int volume = it->second;
        if (hasJob(jobId) && getJob(jobId).getLastVolume() != volume) {
            Console::log(Console::INFO, "#%i : update v=%i", it->first, it->second);
        }
        updateVolume(it->first, it->second);
    }
}

void Worker::allreduceSystemState() {

    static float lastCheck = 0;

    float time = Timer::elapsedSeconds();
    if (time-lastCheck > 0.1) {
        lastCheck = time;
        float timeSinceLast = time-lastSystemStateReduce;
        if (!reducingSystemState && timeSinceLast >= 1.0) {
            lastSystemStateReduce = time;
            myState[0] = isIdle() ? 0.0f : 1.0f;
            myState[1] = currentJob != NULL && currentJob->isRoot() ? 1.0f : 0.0f;
            systemState[0] = 0.0f;
            systemState[1] = 0.0f;
            systemState[2] = 0.0f;
            systemStateReq = MyMpi::iallreduce(comm, myState, systemState, 3);
            reducingSystemState = true;
        } else if (reducingSystemState) {
            MPI_Status status;
            bool done = MyMpi::test(systemStateReq, status);
            if (done) {
                reducingSystemState = false;
                int verb = (worldRank == 0 ? Console::INFO : Console::VVVVERB);
                Console::log(verb, "sysstate busy=%.2f%% jobs=%i accmem=%.2fGB", 100*systemState[0]/MyMpi::size(comm), (int)systemState[1], systemState[2]);
            } else if (lastSystemStateReduce > 0 && timeSinceLast > 300) {
                Console::log(Console::CRIT, "Unresponsive node(s) since 300 seconds! Aborting");
                abort();
            }
        }
    }

}

void Worker::timeoutJob(int jobId) {
    // "Virtual self message" aborting the job
    IntVec payload({jobId, getJob(jobId).getRevision()});
    MessageHandlePtr handle(new MessageHandle(MyMpi::nextHandleId()));
    handle->source = worldRank;
    handle->recvData = payload.serialize();
    handle->tag = MSG_ABORT;
    handle->finished = true;
    handleAbort(handle);
}

bool Worker::checkComputationLimits(int jobId) {

    if (!getJob(jobId).isRoot()) return false;

    if (!lastLimitCheck.count(jobId) || !jobCpuTimeUsed.count(jobId)) {
        // Job is new
        lastLimitCheck[jobId] = Timer::elapsedSeconds();
        jobCpuTimeUsed[jobId] = 0;
        return false;
    }

    float elapsedTime = Timer::elapsedSeconds() - lastLimitCheck[jobId];
    bool terminate = false;
    assert(elapsedTime >= 0);

    // Calculate CPU seconds: (volume during last epoch) * #threads * (effective time of last epoch) 
    float newCpuTime = (jobVolumes.count(jobId) ? jobVolumes[jobId] : 1) * numThreads * elapsedTime;
    assert(newCpuTime >= 0);
    jobCpuTimeUsed[jobId] += newCpuTime;
    bool hasCpuLimit = cpuSecsPerInstance > 0;
    
    if (hasCpuLimit && jobCpuTimeUsed[jobId] > cpuSecsPerInstance) {
        // Job exceeded its cpu time limit
        Console::log(Console::INFO, "#%i CPU TIMEOUT: aborting", jobId);
        terminate = true;

    } else {
        // Calculate wall clock time
        float jobAge = currentJob->getAge();
        if (wcSecsPerInstance > 0 && jobAge > wcSecsPerInstance) {
            // Job exceeded its wall clock time limit
            Console::log(Console::INFO, "#%i WALLCLOCK TIMEOUT: aborting", jobId);
            terminate = true;
        }
    }
    
    if (terminate) lastLimitCheck.erase(jobId);
    else lastLimitCheck[jobId] = Timer::elapsedSeconds();
    
    return terminate;
}

void Worker::updateVolume(int jobId, int volume) {

    if (!hasJob(jobId)) return;
    Job &job = getJob(jobId);

    if (!job.isActive()) {
        // Job is not active right now
        return;
    }

    // Root node update message
    int thisIndex = job.getIndex();
    if (thisIndex == 0 && job.getLastVolume() != volume) {
        Console::log(Console::VERB, "%s : update v=%i", job.toStr(), volume);
    }
    job.setLastVolume(volume);

    // Prepare volume update to propagate down the job tree
    IntPair payload(jobId, volume);

    std::set<int> dormantChildren = job.getDormantChildren();

    // For each potential child (left, right):
    bool has[2] = {job.hasLeftChild(), job.hasRightChild()};
    int indices[2] = {job.getLeftChildIndex(), job.getRightChildIndex()};
    int ranks[2] = {-1, -1};
    for (int i = 0; i < 2; i++) {
        int nextIndex = indices[i];
        if (has[i]) {
            ranks[i] = i == 0 ? job.getLeftChildNodeRank() : job.getRightChildNodeRank();
            // Propagate volume update
            MyMpi::isend(MPI_COMM_WORLD, ranks[i], MSG_UPDATE_VOLUME, payload);
            //stats.increment("sentMessages");

            // Done over MSG_WORKER_DEFECTING now
            /* 
            if (nextIndex >= volume) {
                // Prune child
                Console::log_send(Console::VERB, ranks[i], "%s : prune child %s", 
                        job.toStr(), jobStr(job.getId(), nextIndex).c_str());
                if (i == 0) job.unsetLeftChild();
                if (i == 1) job.unsetRightChild();
            }*/

        } else if (job.hasJobDescription() && nextIndex < volume && !jobCommitments.count(jobId)) {
            // Grow
            Console::log(Console::VVVERB, "%s : grow", job.toStr());
            JobRequest req(jobId, job.getRootNodeRank(), worldRank, nextIndex, Timer::elapsedSeconds(), 0);
            int nextNodeRank, tag;
            if (dormantChildren.empty()) {
                tag = MSG_FIND_NODE;
                ranks[i] = i == 0 ? job.getLeftChildNodeRank() : job.getRightChildNodeRank();
                nextNodeRank = ranks[i];
            } else {
                tag = MSG_FIND_NODE_ONESHOT;
                nextNodeRank = Random::choice(dormantChildren);
                dormantChildren.erase(nextNodeRank);
            }
            Console::log_send(Console::VERB, nextNodeRank, "%s : initiate growth by %s", 
                        job.toStr(), jobStr(job.getId(), nextIndex).c_str());
            MyMpi::isend(MPI_COMM_WORLD, nextNodeRank, tag, req);
            //stats.increment("sentMessages");
        }
    }

    // Shrink (and pause solving) if necessary
    if (thisIndex > 0 && thisIndex >= volume) {
        MyMpi::isend(MPI_COMM_WORLD, job.getParentNodeRank(), MSG_WORKER_DEFECTING, IntPair(jobId, thisIndex));
        job.suspend();
        setLoad(0, jobId);
    }
}

bool Worker::isTimeForRebalancing() {
    return epochCounter.getSecondsSinceLastSync() >= balancePeriod;
}

bool Worker::isRequestObsolete(const JobRequest& req) {

    // Requests for a job root never become obsolete
    if (req.requestedNodeIndex == 0) return false;

    // Does this node KNOW that the request is already completed?
    if (!isIdle() && req.jobId == currentJob->getId()) {
        Job& job = getJob(req.jobId);
        if (req.requestedNodeIndex == job.getIndex()
        || (job.hasLeftChild() && req.requestedNodeIndex == job.getLeftChildIndex())
        || (job.hasRightChild() && req.requestedNodeIndex == job.getRightChildIndex())) {
            // Request completed!
            Console::log(Console::VERB, "Req. %s : already completed", job.toStr());
            return true;
        }
    }
    
    float timelim = 0.25 + 2 * params.getFloatParam("p");

    // Make requests in single instance mode last for a long time
    if (params.isSet("sinst")) timelim = std::max(timelim, 300.0f);

    return Timer::elapsedSeconds() - req.timeOfBirth >= timelim; 
}

bool Worker::isAdoptionOfferObsolete(const JobRequest& req) {

    // Requests for a job root never become obsolete
    if (req.requestedNodeIndex == 0) return false;

    // Job not known anymore: obsolete
    if (!hasJob(req.jobId)) return true;

    Job& job = getJob(req.jobId);
    if (!job.isActive()) {
        // Job is not active
        Console::log(Console::VERB, "Req. %s : job inactive (%s)", job.toStr(), job.jobStateToStr());
        return true;
    
    } else if (req.requestedNodeIndex == job.getLeftChildIndex() && job.hasLeftChild()) {
        // Job already has a left child
        Console::log(Console::VERB, "Req. %s : already has left child", job.toStr());
        return true;

    } else if (req.requestedNodeIndex == job.getRightChildIndex() && job.hasRightChild()) {
        // Job already has a right child
        Console::log(Console::VERB, "Req. %s : already has right child", job.toStr());
        return true;
    }

    return false;
}

Job* Worker::createJob(Parameters& params, int commSize, int worldRank, int jobId, EpochCounter& epochCounter) {
    if (params.getParam("appmode") == "fork") {
        return new ForkedSatJob(params, commSize, worldRank, jobId, epochCounter);
    } else {
        return new ThreadedSatJob(params, commSize, worldRank, jobId, epochCounter);
    }
}

struct SuspendedJobComparator {
    bool operator()(const std::pair<int, float>& left, const std::pair<int, float>& right) {
        return left.second < right.second;
    };
};

void Worker::forgetOldJobs() {

    std::vector<int> jobsToForget;
    int jobCacheSize = params.getIntParam("jc");

    // Scan jobs for being forgettable
    std::priority_queue<std::pair<int, float>, std::vector<std::pair<int, float>>, SuspendedJobComparator> suspendedQueue;
    for (auto idJobPair : jobs) {
        int id = idJobPair.first;
        Job& job = *idJobPair.second;
        // Job must be finished initializing
        if (job.isInitializing()) {
            // Check end of initialization for inactive jobs
            if (!job.isActive() && job.isDoneInitializing()) job.endInitialization();
            continue;
        }
        if (jobCacheSize > 0 && job.isSuspended()) {
            // Job must not be rooted here
            if (job.isRoot()) continue;
            // Insert job into PQ according to its age 
            float age = job.getAgeSinceActivation();
            suspendedQueue.emplace(id, age);
        }
        if (job.isPast() || job.isForgetting()) {
            // If job is past, it must have been so for at least 60 seconds
            if (job.getAgeSinceAbort() < 60) continue;
            // If the node found a result, it must have been already transferred
            if (job.isResultTransferPending()) continue;
            jobsToForget.push_back(id);
        }
    }

    // Mark jobs as forgettable as long as job cache is exceeded
    while (suspendedQueue.size() > jobCacheSize) {
        jobsToForget.push_back(suspendedQueue.top().first);
        suspendedQueue.pop();
    }

    // Perform forgetting of jobs
    for (int jobId : jobsToForget) {
        forgetJob(jobId);
    }
}

void Worker::forgetJob(int jobId) {
    Job& job = getJob(jobId);
    Console::log(Console::VVVERB, "Terminate %s to forget", job.toStr());
    if (job.isSuspended()) {
        job.stop();
        job.terminate();
    }
    job.setForgetting();
    // Check if the job can be destructed
    if (job.isDestructible()) {
        deleteJob(jobId);
        Console::log(Console::VVVERB, "Forgot #%i", jobId);
    }
}

void Worker::deleteJob(int jobId) {

    if (!hasJob(jobId)) return;
    Job& job = getJob(jobId);
    int index = job.getIndex();
    Console::log(Console::VVERB, "Delete %s", job.toStr());

    // Join and delete initializer thread
    if (initializerThreads.count(jobId)) {
        Console::log(Console::VVVERB, "Delete init thread of %s", job.toStr());
        if (initializerThreads[jobId].joinable()) {
            initializerThreads[jobId].join();
        }
    }

    // Remove map entries
    jobCommitments.erase(jobId);
    jobArrivals.erase(jobId);
    jobCpuTimeUsed.erase(jobId);  
    lastLimitCheck.erase(jobId);      
    jobVolumes.erase(jobId);  
    initializerThreads.erase(jobId);

    // Remove job meta data from balancer
    balancer->forget(jobId);

    // Delete job and its solvers
    jobs.erase(jobId);
    delete &job;

    Console::log(Console::VERB, "Deleted %s", jobStr(jobId, index).c_str());
}

Worker::~Worker() {

    exiting = true;

    // Send termination signal to the entire process group (quicker than normal terminate)
    // (Workaround for idle times after finishing)
    Fork::terminateAll();
    kill(getpid(), SIGTERM);

    // Delete each job (iterating over "jobs" invalid as entries are deleted)
    std::vector<int> jobIds;
    for (auto idJobPair : jobs) jobIds.push_back(idJobPair.first);
    for (int jobId : jobIds) deleteJob(jobId);

    if (mpiMonitorThread.joinable())
        mpiMonitorThread.join();

    Console::log(Console::VVERB, "Destruct worker");
}