
#include <cmath>
#include <thread>
#include <unistd.h>
#include <iostream>
#include <fstream>
#include <initializer_list>
#include <limits>

#include "worker.hpp"

#include "app/sat/threaded_sat_job.hpp"
#include "app/sat/forked_sat_job.hpp"
#include "app/sat/sat_constants.h"

#include "balancing/event_driven_balancer.hpp"
#include "comm/mpi_monitor.hpp"
#include "data/serializable.hpp"
#include "data/job_description.hpp"
#include "util/sys/process.hpp"
#include "util/sys/proc.hpp"
#include "util/sys/timer.hpp"
#include "util/sys/watchdog.hpp"
#include "util/console.hpp"
#include "util/random.hpp"
#include "util/sat_reader.hpp"

void Worker::init() {
    
    // Initialize pseudo-random order of nodes
    if (_params.isNotNull("derandomize")) {
        createExpanderGraph();
    }

    // Begin listening to an incoming message
    _msg_handler.registerCallback(MSG_ACCEPT_ADOPTION_OFFER, 
        [&](auto& h) {handleAcceptAdoptionOffer(h);});
    _msg_handler.registerCallback(MSG_CONFIRM_ADOPTION, 
        [&](auto& h) {handleConfirmAdoption(h);});
    _msg_handler.registerCallback(MSG_CONFIRM_JOB_REVISION_DETAILS, 
        [&](auto& h) {handleConfirmJobRevisionDetails(h);});
    _msg_handler.registerCallback(MSG_DO_EXIT, 
        [&](auto& h) {handleDoExit(h);});
    _msg_handler.registerCallback(MSG_NOTIFY_JOB_ABORTING, 
        [&](auto& h) {handleNotifyJobAborting(h);});
    _msg_handler.registerCallback(MSG_NOTIFY_JOB_DONE, 
        [&](auto& h) {handleNotifyJobDone(h);});
    _msg_handler.registerCallback(MSG_NOTIFY_JOB_REVISION, 
        [&](auto& h) {handleNotifyJobRevision(h);});
    _msg_handler.registerCallback(MSG_NOTIFY_JOB_TERMINATING, 
        [&](auto& h) {handleNotifyJobTerminating(h);});
    _msg_handler.registerCallback(MSG_NOTIFY_NODE_LEAVING_JOB, 
        [&](auto& h) {handleNotifyNodeLeavingJob(h);});
    _msg_handler.registerCallback(MSG_NOTIFY_RESULT_FOUND, 
        [&](auto& h) {handleNotifyResultFound(h);});
    _msg_handler.registerCallback(MSG_NOTIFY_RESULT_OBSOLETE, 
        [&](auto& h) {handleNotifyResultObsolete(h);});
    _msg_handler.registerCallback(MSG_NOTIFY_VOLUME_UPDATE, 
        [&](auto& h) {handleNotifyVolumeUpdate(h);});
    _msg_handler.registerCallback(MSG_OFFER_ADOPTION, 
        [&](auto& h) {handleOfferAdoption(h);});
    _msg_handler.registerCallback(MSG_QUERY_JOB_RESULT, 
        [&](auto& h) {handleQueryJobResult(h);});
    _msg_handler.registerCallback(MSG_QUERY_JOB_REVISION_DETAILS, 
        [&](auto& h) {handleQueryJobRevisionDetails(h);});
    _msg_handler.registerCallback(MSG_QUERY_VOLUME, 
        [&](auto& h) {handleQueryVolume(h);});
    _msg_handler.registerCallback(MSG_REJECT_ADOPTION_OFFER, 
        [&](auto& h) {handleRejectAdoptionOffer(h);});
    _msg_handler.registerCallback(MSG_REJECT_ONESHOT, 
        [&](auto& h) {handleRejectOneshot(h);});
    _msg_handler.registerCallback(MSG_REQUEST_NODE, 
        [&](auto& h) {handleRequestNode(h, /*oneshot=*/false);});
    _msg_handler.registerCallback(MSG_REQUEST_NODE_ONESHOT, 
        [&](auto& h) {handleRequestNode(h, /*oneshot=*/true);});
    _msg_handler.registerCallback(MSG_SEND_APPLICATION_MESSAGE, 
        [&](auto& h) {handleSendApplicationMessage(h);});
    _msg_handler.registerCallback(MSG_SEND_CLIENT_RANK, 
        [&](auto& h) {handleSendClientRank(h);});
    _msg_handler.registerCallback(MSG_SEND_JOB_DESCRIPTION, 
        [&](auto& h) {handleSendJob(h);});
    _msg_handler.registerCallback(MSG_SEND_JOB_REVISION_DETAILS, 
        [&](auto& h) {handleSendJobRevisionDetails(h);});
    _msg_handler.registerCallback(MSG_SEND_JOB_REVISION_DATA, 
        [&](auto& h) {handleSendJobRevisionData(h);});
    _msg_handler.registerCallback(MSG_SEND_JOB_RESULT, 
        [&](auto& h) {handleSendJobResult(h);});
    auto balanceCb = [&](MessageHandle& handle) {
        if (_job_db.continueBalancing(handle)) applyBalancing();
    };
    _msg_handler.registerCallback(MSG_COLLECTIVE_OPERATION, balanceCb);
    _msg_handler.registerCallback(MSG_REDUCE_DATA, balanceCb);
    _msg_handler.registerCallback(MSG_BROADCAST_DATA, balanceCb);
    _msg_handler.registerCallback(MSG_WARMUP, [&](auto& h) {
        Console::log_recv(Console::VVVERB, h.source, "Warmup msg");
    });
    _msg_handler.registerCallback(MessageHandler::TAG_DEFAULT, [&](auto& h) {
        Console::log_recv(Console::WARN, h.source, "[WARN] Unknown message tag %i", h.tag);
    });
    MyMpi::beginListening();

    // Send warm-up messages with your pseudorandom bounce destinations
    if (_params.isNotNull("derandomize") && _params.isNotNull("warmup")) {
        IntVec payload({1, 2, 3, 4, 5, 6, 7, 8});
        int numRuns = 5;
        for (int run = 0; run < numRuns; run++) {
            for (auto rank : _hop_destinations) {
                MyMpi::isend(MPI_COMM_WORLD, rank, MSG_WARMUP, payload);
                Console::log_send(Console::VVERB, rank, "Warmup msg");
            }
        }
    }

    Console::log(Console::VVERB, "Global init barrier ...");
    MPI_Barrier(MPI_COMM_WORLD);
    Console::log(Console::VVERB, "Passed global init barrier");

    if (!MyMpi::_monitor_off) _mpi_monitor_thread = std::thread(mpiMonitor, this);
    
    // Initiate single instance solving as the "root node"
    if (_params.isNotNull("mono") && _world_rank == 0) {

        std::string instanceFilename = _params.getParam("mono");
        Console::log(Console::INFO, "Initiate solving of mono instance \"%s\"", instanceFilename.c_str());

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
        _job_db.createJob(MyMpi::size(_comm), _world_rank, jobId);
        JobRequest req(jobId, 0, 0, 0, 0, 0);
        _job_db.commit(req);
        _job_db.init(jobId, desc.serialize(), _world_rank);
    }
}

void Worker::mainProgram() {

    int iteration = 0;
    float lastMemCheckTime = Timer::elapsedSeconds();
    float lastJobCheckTime = lastMemCheckTime;
    float lastBalanceCheckTime = lastMemCheckTime;

    float sleepMicrosecs = _params.getFloatParam("sleep");
    float memCheckPeriod = 3.0;
    float jobCheckPeriod = 0.01;
    float balanceCheckPeriod = 0.01;
    bool doYield = _params.isNotNull("yield");

    Watchdog watchdog(/*checkIntervMillis=*/60*1000, lastMemCheckTime);

    float time = lastMemCheckTime;
    while (!checkTerminate(time)) {
        
        // Poll received messages, make progress in sent messages
        _msg_handler.pollMessages(time);
        MyMpi::testSentHandles();

        if (time - lastMemCheckTime > memCheckPeriod) {
            lastMemCheckTime = time;
            // Print stats

            // For this process and subprocesses
            auto info = Proc::getRuntimeInfo(Proc::getPid(), Proc::SubprocessMode::RECURSE);
            info.vmUsage *= 0.001 * 0.001;
            info.residentSetSize *= 0.001 * 0.001;
            Console::log(Console::VVERB, "mainthread_cpu=%i", info.cpu);
            Console::log(Console::VERB, "mem=%.2fGB", info.residentSetSize);
            _sys_state.setLocal(SYSSTATE_GLOBALMEM, info.residentSetSize);

            // For this "management" thread
            double cpuShare; float sysShare;
            bool success = Proc::getThreadCpuRatio(Proc::getTid(), cpuShare, sysShare);
            if (success) {
                Console::log(Console::VERB, "mainthread cpuratio=%.3f sys=%.3f", cpuShare, sysShare);
            }

            // For the current job
            if (!_job_db.isIdle()) _job_db.getActive().appl_dumpStats();

            // Forget jobs that are old or wasting memory
            _job_db.forgetOldJobs();

            // Reset watchdog
            watchdog.reset(time);
        }

        // Advance load balancing operations
        if (time - lastBalanceCheckTime > balanceCheckPeriod) {
            lastBalanceCheckTime = time;
            if (_job_db.isTimeForRebalancing()) {
                if (_job_db.beginBalancing()) applyBalancing();
            } else if (_job_db.continueBalancing()) applyBalancing();
        } 

        // Check active job
        if (time - lastJobCheckTime >= jobCheckPeriod) {
            lastJobCheckTime = time;

            if (_job_db.isIdle()) {
                _sys_state.setLocal(SYSSTATE_BUSYRATIO, 0.0f); // busy nodes
                _sys_state.setLocal(SYSSTATE_NUMJOBS, 0.0f); // active jobs

            } else {
                Job &job = _job_db.getActive();
                int id = job.getId();
                bool isRoot = job.getJobTree().isRoot();

                _sys_state.setLocal(SYSSTATE_BUSYRATIO, 1.0f); // busy nodes
                _sys_state.setLocal(SYSSTATE_NUMJOBS, isRoot ? 1.0f : 0.0f); // active jobs

                bool abort = false;
                if (isRoot) abort = _job_db.checkComputationLimits(id);
                if (abort) {
                    // Timeout (CPUh or wallclock time) hit
                    timeoutJob(id);
                } else if (job.getState() == ACTIVE) {
                    
                    if (job.testReadyToGrow()) {
                        if (job.getJobTree().isRoot()) {
                            // Root worker: update volume again (to trigger growth)
                            if (job.getVolume() > 1) updateVolume(id, job.getVolume());
                        } else {
                            // Non-root worker: query parent for the volume of this job
                            IntVec payload({id});
                            MyMpi::isend(MPI_COMM_WORLD, job.getJobTree().getParentNodeRank(), MSG_QUERY_VOLUME, payload);
                        }
                    }

                    // Check if a result was found
                    int result = job.appl_solved();
                    if (result >= 0) {
                        // Solver done!
                        // Signal termination to root -- may be a self message
                        int jobRootRank = job.getJobTree().getRootNodeRank();
                        IntVec payload({job.getId(), job.getRevision(), result});
                        Console::log_send(Console::VVERB, jobRootRank, "%s : sending finished info", job.toStr());
                        MyMpi::isend(MPI_COMM_WORLD, jobRootRank, MSG_NOTIFY_RESULT_FOUND, payload);
                        job.setResultTransferPending(true);
                    }
                }

                // Job communication (e.g. clause sharing)
                if (job.wantsToCommunicate()) job.communicate();
            }

        }

        // Advance an all-reduction of the current system state
        if (_sys_state.aggregate(time)) {
            float* result = _sys_state.getGlobal();
            int verb = (_world_rank == 0 ? Console::INFO : Console::VVVVERB);
            Console::log(verb, "sysstate busyratio=%.3f jobs=%i globmem=%.2fGB hops=%i", 
                        result[0]/MyMpi::size(_comm), (int)result[1], result[2], (int)result[3]);
            _sys_state.setLocal(SYSSTATE_NUMHOPS, 0); // reset #hops
        }

        if (sleepMicrosecs > 0) usleep(sleepMicrosecs);
        if (doYield) std::this_thread::yield();

        time = Timer::elapsedSeconds();
    }

    watchdog.stop();
    Console::flush();
    fflush(stdout);
}

void Worker::handleNotifyJobAborting(MessageHandle& handle) {

    int jobId = Serializable::get<int>(handle.recvData);
    if (!_job_db.has(jobId)) return;

    interruptJob(jobId, /*terminate=*/true, /*reckless=*/true);
    
    if (_job_db.get(jobId).getJobTree().isRoot()) {
        // Forward information on aborted job to client
        MyMpi::isend(MPI_COMM_WORLD, _job_db.get(jobId).getJobTree().getParentNodeRank(), 
            MSG_NOTIFY_JOB_ABORTING, handle.recvData);
    }
}

void Worker::handleAcceptAdoptionOffer(MessageHandle& handle) {

    // Retrieve according job commitment
    JobSignature sig = Serializable::get<JobSignature>(handle.recvData);
    if (!_job_db.hasCommitment(sig.jobId)) {
        Console::log(Console::WARN, "[WARN] Job commitment for #%i not present despite adoption accept msg", sig.jobId);
        return;
    }

    const JobRequest& req = _job_db.getCommitment(sig.jobId);

    if (req.fullTransfer == 1) {
        // Full transfer of job description is required:
        // Send ACK to parent and receive full job description
        Console::log(Console::VVERB, "Will receive desc. of #%i, size %i", req.jobId, sig.getTransferSize());
        MyMpi::irecv(MPI_COMM_WORLD, handle.source, MSG_SEND_JOB_DESCRIPTION, sig.getTransferSize()); // to be received later
        MyMpi::isend(MPI_COMM_WORLD, handle.source, MSG_CONFIRM_ADOPTION, req);
    } else {
        _job_db.uncommit(req.jobId);
        _job_db.reactivate(req, handle.source);
        // Query parent for current volume of job
        MyMpi::isend(MPI_COMM_WORLD, handle.source, MSG_QUERY_VOLUME, IntVec({req.jobId}));
    }
}

void Worker::handleConfirmJobRevisionDetails(MessageHandle& handle) {
    IntVec response = Serializable::get<IntVec>(handle.recvData);
    int jobId = response[0];
    int firstRevision = response[1];
    int lastRevision = response[2];
    MyMpi::isend(MPI_COMM_WORLD, handle.source, MSG_SEND_JOB_REVISION_DATA, 
                _job_db.get(jobId).getDescription().serialize(firstRevision, lastRevision));
}

void Worker::handleConfirmAdoption(MessageHandle& handle) {
    JobRequest req = Serializable::get<JobRequest>(handle.recvData);

    // If job offer is obsolete, send a stub description containing the job id ONLY
    if (_job_db.isAdoptionOfferObsolete(req, /*alreadyAccepted=*/true)) {
        // Obsolete request
        Console::log_recv(Console::VERB, handle.source, "REJECT r.%s birth=%.2f hops=%i", 
                            _job_db.toStr(req.jobId, req.requestedNodeIndex).c_str(), 
                            req.timeOfBirth, req.numHops);
        MyMpi::isend(MPI_COMM_WORLD, handle.source, MSG_SEND_JOB_DESCRIPTION, IntVec({req.jobId}));
        return;
    }
    Job& job = _job_db.get(req.jobId);

    // Retrieve and send concerned job description
    MyMpi::isend(MPI_COMM_WORLD, handle.source, MSG_SEND_JOB_DESCRIPTION, job.getSerializedDescription());
    Console::log_send(Console::VVERB, handle.source, "Sent job desc. of %s", job.toStr());

    // Mark new node as one of the node's children
    auto relative = job.getJobTree().setChild(handle.source, req.requestedNodeIndex);
    if (relative == JobTree::TreeRelative::NONE) assert(req.requestedNodeIndex == 0);
}

void Worker::handleDoExit(MessageHandle& handle) {
    Console::log_recv(Console::VERB, handle.source, "Received exit signal");

    // Forward exit signal
    if (_world_rank*2+1 < MyMpi::size(MPI_COMM_WORLD))
        MyMpi::isend(MPI_COMM_WORLD, _world_rank*2+1, MSG_DO_EXIT, handle.recvData);
    if (_world_rank*2+2 < MyMpi::size(MPI_COMM_WORLD))
        MyMpi::isend(MPI_COMM_WORLD, _world_rank*2+2, MSG_DO_EXIT, handle.recvData);
    while (MyMpi::hasOpenSentHandles()) MyMpi::testSentHandles();

    _exiting = true;
}

void Worker::handleRejectOneshot(MessageHandle& handle) {
    JobRequest req = Serializable::get<JobRequest>(handle.recvData);
    Console::log_recv(Console::VVVERB, handle.source, 
        "%s rejected by dormant child", _job_db.toStr(req.jobId, req.requestedNodeIndex).c_str());

    if (_job_db.isAdoptionOfferObsolete(req)) return;

    Job& job = _job_db.get(req.jobId);
    job.getJobTree().addFailToDormantChild(handle.source);

    bool doNormalHopping = false;
    if (req.numHops > std::max(_params.getIntParam("jc"), 2)) {
        // Oneshot node finding exceeded
        doNormalHopping = true;
    } else {
        // Attempt another oneshot request
        req.numHops++;
        // Get dormant children without the node that just declined
        std::set<int> dormantChildren = job.getJobTree().getDormantChildren();
        if (dormantChildren.count(handle.source)) dormantChildren.erase(handle.source);
        if (dormantChildren.empty()) {
            // No fitting dormant children left
            doNormalHopping = true;
        } else {
            // Pick a dormant child, forward request
            int rank = Random::choice(dormantChildren);
            MyMpi::isend(MPI_COMM_WORLD, rank, MSG_REQUEST_NODE_ONESHOT, req);
            Console::log_send(Console::VVERB, rank, "%s : query dormant child", job.toStr());
        }
    }

    if (doNormalHopping) {
        Console::log(Console::VVERB, "%s : switch to normal hops", job.toStr());
        req.numHops = -1;
        bounceJobRequest(req, handle.source);
    }
}

void Worker::handleRequestNode(MessageHandle& handle, bool oneshot) {

    JobRequest req = Serializable::get<JobRequest>(handle.recvData);

    // Discard request if it has become obsolete
    if (_job_db.isRequestObsolete(req)) {
        Console::log_recv(Console::VERB, handle.source, "DISCARD r.%s birth=%.2f hops=%i oneshot=%i", 
                _job_db.toStr(req.jobId, req.requestedNodeIndex).c_str(), req.timeOfBirth, req.numHops, oneshot ? 1 : 0);
        return;
    }

    int removedJob;
    bool adopts = _job_db.tryAdopt(req, oneshot, removedJob);
    if (adopts) {
        if (removedJob >= 0) {
            Job& job = _job_db.get(removedJob);
            IntPair pair(job.getId(), job.getIndex());
            MyMpi::isend(MPI_COMM_WORLD, job.getJobTree().getParentNodeRank(), MSG_NOTIFY_NODE_LEAVING_JOB, pair);
        }

        // Adoption takes place
        std::string jobstr = _job_db.toStr(req.jobId, req.requestedNodeIndex);
        Console::log_recv(Console::VERB, handle.source, "ADOPT r.%s birth=%.2f hops=%i oneshot=%i", jobstr.c_str(), 
                req.timeOfBirth, req.numHops, oneshot ? 1 : 0);
        assert(_job_db.isIdle() || Console::fail("Adopting a job, but not idle!"));

        // Commit on the job, send a request to the parent
        bool fullTransfer = false;
        if (!_job_db.has(req.jobId)) {
            // Job is not known yet: create instance, request full transfer
            _job_db.createJob(MyMpi::size(_comm), _world_rank, req.jobId);
            fullTransfer = true;
        } else if (!_job_db.get(req.jobId).hasReceivedDescription()) {
            // Job is known, but never received full description:
            // request full transfer
            fullTransfer = true;
        }
        req.fullTransfer = fullTransfer ? 1 : 0;
        _job_db.commit(req);
        MyMpi::isend(MPI_COMM_WORLD, req.requestingNodeRank, MSG_OFFER_ADOPTION, req);

    } else {
        if (oneshot) {
            Console::log_send(Console::VVVERB, handle.source, "decline oneshot request for %s", 
                        _job_db.toStr(req.jobId, req.requestedNodeIndex).c_str());
            MyMpi::isend(MPI_COMM_WORLD, handle.source, MSG_REJECT_ONESHOT, handle.recvData);
        } else {
            // Continue job finding procedure somewhere else
            bounceJobRequest(req, handle.source);
        }
    }
}

void Worker::handleSendClientRank(MessageHandle& handle) {

    // Receive rank of the job's client
    IntPair recv = Serializable::get<IntPair>(handle.recvData);
    int jobId = recv.first;
    int clientRank = recv.second;
    assert(_job_db.has(jobId));

    // Inform client of the found job result
    informClientJobIsDone(jobId, clientRank);
}

void Worker::handleIncrementalJobFinished(MessageHandle& handle) {
    interruptJob(Serializable::get<int>(handle.recvData), /*terminate=*/true, /*reckless=*/false);
}

void Worker::handleInterrupt(MessageHandle& handle) {
    interruptJob(Serializable::get<int>(handle.recvData), /*terminate=*/false, /*reckless=*/false);
}

void Worker::handleSendApplicationMessage(MessageHandle& handle) {

    // Deserialize job-specific message
    JobMessage msg = Serializable::get<JobMessage>(handle.recvData);
    int jobId = msg.jobId;
    if (!_job_db.has(jobId)) {
        Console::log(Console::WARN, "[WARN] Job message from unknown job #%i", jobId);
        return;
    }
    // Give message to corresponding job
    Job& job = _job_db.get(jobId);
    if (job.getState() == ACTIVE) job.appl_communicate(handle.source, msg);
}

void Worker::handleNotifyJobDone(MessageHandle& handle) {
    IntPair recv = Serializable::get<IntPair>(handle.recvData);
    int jobId = recv.first;
    int resultSize = recv.second;
    Console::log_recv(Console::VVERB, handle.source, "Will receive job result, length %i, for job #%i", resultSize, jobId);
    MyMpi::irecv(MPI_COMM_WORLD, handle.source, MSG_SEND_JOB_RESULT, resultSize);
    MyMpi::isend(MPI_COMM_WORLD, handle.source, MSG_QUERY_JOB_RESULT, handle.recvData);
}

void Worker::handleNotifyJobRevision(MessageHandle& handle) {
    IntVec payload = Serializable::get<IntVec>(handle.recvData);
    int jobId = payload[0];
    int revision = payload[1];

    assert(_job_db.has(jobId));

    // Request receiving information on revision size
    Job& job = _job_db.get(jobId);
    int lastKnownRevision = job.getRevision();
    if (revision > lastKnownRevision) {
        Console::log(Console::VERB, "Received revision update #%i rev. %i (I am at rev. %i)", jobId, revision, lastKnownRevision);
        IntVec request({jobId, lastKnownRevision+1, revision});
        MyMpi::isend(MPI_COMM_WORLD, handle.source, MSG_QUERY_JOB_REVISION_DETAILS, request);
    } else {
        // TODO ???
        Console::log(Console::WARN, "[WARN] Useless revision update #%i rev. %i (I am already at rev. %i)", jobId, revision, lastKnownRevision);
    }
}

void Worker::handleOfferAdoption(MessageHandle& handle) {

    JobRequest req = Serializable::get<JobRequest>(handle.recvData);
    Console::log_recv(Console::VVERB, handle.source, "Adoption offer for %s", 
                    _job_db.toStr(req.jobId, req.requestedNodeIndex).c_str());

    bool reject = false;
    if (!_job_db.has(req.jobId)) {
        reject = true;

    } else {
        // Retrieve concerned job
        Job &job = _job_db.get(req.jobId);

        // Check if node should be adopted or rejected
        if (_job_db.isAdoptionOfferObsolete(req)) {
            // Obsolete request
            Console::log_recv(Console::VERB, handle.source, "REJECT r.%s birth=%.2f hops=%i", 
                                job.toStr(), req.timeOfBirth, req.numHops);
            reject = true;

        } else {
            // Adopt the job
            const JobDescription& desc = job.getDescription();

            // Send job signature
            JobSignature sig(req.jobId, req.rootRank, req.revision, desc.getTransferSize(true));
            MyMpi::isend(MPI_COMM_WORLD, handle.source, MSG_ACCEPT_ADOPTION_OFFER, sig);

            // If req.fullTransfer, then wait for the child to acknowledge having received the signature
            if (req.fullTransfer == 1) {
                Console::log_send(Console::VVERB, handle.source, "Will send desc. of %s", 
                        _job_db.toStr(req.jobId, req.requestedNodeIndex).c_str());
            } else {
                Console::log_send(Console::VVERB, handle.source, "Resume child %s", 
                        _job_db.toStr(req.jobId, req.requestedNodeIndex).c_str());
            }
            // Child will start / resume its job solvers.
            // Mark new node as one of the node's children
            auto relative = job.getJobTree().setChild(handle.source, req.requestedNodeIndex);
            if (relative == JobTree::TreeRelative::NONE) assert(req.requestedNodeIndex == 0);
        }
    }

    // If rejected: Send message to rejected child node
    if (reject) {
        MyMpi::isend(MPI_COMM_WORLD, handle.source, MSG_REJECT_ADOPTION_OFFER, req);
    }
}

void Worker::handleQueryJobResult(MessageHandle& handle) {

    // Receive acknowledgement that the client received the advertised result size
    // and wishes to receive the full job result
    int jobId = Serializable::get<int>(handle.recvData);
    assert(_job_db.has(jobId));
    const JobResult& result = _job_db.get(jobId).getResult();
    Console::log_send(Console::VERB, handle.source, "Send full result to client");
    MyMpi::isend(MPI_COMM_WORLD, handle.source, MSG_SEND_JOB_RESULT, result);
    _job_db.get(jobId).setResultTransferPending(false);
}

void Worker::handleQueryJobRevisionDetails(MessageHandle& handle) {
    IntVec request = Serializable::get<IntVec>(handle.recvData);
    int jobId = request[0];
    int firstRevision = request[1];
    int lastRevision = request[2];

    assert(_job_db.has(jobId));

    const JobDescription& desc = _job_db.get(jobId).getDescription();
    IntVec response({jobId, firstRevision, lastRevision, desc.getTransferSize(firstRevision, lastRevision)});
    MyMpi::isend(MPI_COMM_WORLD, handle.source, MSG_SEND_JOB_REVISION_DETAILS, response);
}

void Worker::handleQueryVolume(MessageHandle& handle) {

    IntVec payload = Serializable::get<IntVec>(handle.recvData);
    int jobId = payload[0];

    // No volume of this job (yet?) -- ignore.
    if (!_job_db.has(jobId)) return;

    int volume = _job_db.get(jobId).getVolume();
    IntVec response({jobId, volume});

    Console::log_send(Console::VVERB, handle.source, "Answer #%i volume query with v=%i", jobId, volume);
    MyMpi::isend(MPI_COMM_WORLD, handle.source, MSG_NOTIFY_VOLUME_UPDATE, response);
}

void Worker::handleRejectAdoptionOffer(MessageHandle& handle) {

    // Retrieve committed job
    JobRequest req = Serializable::get<JobRequest>(handle.recvData);
    if (!_job_db.has(req.jobId)) return;
    Job &job = _job_db.get(req.jobId);

    // Erase commitment
    Console::log_recv(Console::VVERB, handle.source, "Rejected to become %s : uncommitting", job.toStr());
    _job_db.uncommit(req.jobId);
}

void Worker::handleNotifyResultObsolete(MessageHandle& handle) {
    IntVec res = Serializable::get<IntVec>(handle.recvData);
    int jobId = res[0];
    //int revision = res[1];
    if (!_job_db.has(jobId)) return;
    Console::log_recv(Console::VVERB, handle.source, "job result for %s unwanted", _job_db.get(jobId).toStr());
    _job_db.get(jobId).setResultTransferPending(false);
}

void Worker::handleSendJob(MessageHandle& handle) {
    auto& data = handle.recvData;
    Console::log_recv(Console::VVVERB, handle.source, "Receiving some desc. of size %i", data.size());
    int jobId = Serializable::get<int>(data);
    _job_db.init(jobId, std::move(data), handle.source);
}

void Worker::handleSendJobResult(MessageHandle& handle) {
    JobResult jobResult = Serializable::get<JobResult>(handle.recvData);
    int jobId = jobResult.id;
    int resultCode = jobResult.result;
    int revision = jobResult.revision;

    Console::log_recv(Console::INFO, handle.source, "Received result of job #%i rev. %i, code: %i", jobId, revision, resultCode);
    Console::log_noprefix(Console::CRIT, "s %s", resultCode == RESULT_SAT ? "SATISFIABLE" 
                            : resultCode == RESULT_UNSAT ? "UNSATISFIABLE" : "UNKNOWN");
    if (resultCode == RESULT_SAT) {
        std::string model = "";
        for (size_t x = 1; x < jobResult.solution.size(); x++) {
            model += std::to_string(jobResult.solution[x]) + " ";
        }
        Console::log_noprefix(Console::CRIT, "v %s", model.c_str());
    }

    if (_params.isNotNull("mono")) {
        // Single instance solving is done: begin exit signal
        MyMpi::isend(MPI_COMM_WORLD, 0, MSG_DO_EXIT, IntVec({0}));
    }
}

void Worker::handleSendJobRevisionData(MessageHandle& handle) {
    int jobId = Serializable::get<int>(handle.recvData);
    assert(_job_db.has(jobId));

    // TODO in separate thread
    Job& job = _job_db.get(jobId);
    //job.unpackAmendment(handle.recvData);
    int revision = job.getDescription().getRevision();
    Console::log(Console::INFO, "%s : computing on #%i rev. %i", job.toStr(), jobId, revision);
    
    // Propagate to children
    if (job.getJobTree().hasLeftChild()) {
        IntVec payload({jobId, revision});
        MyMpi::isend(MPI_COMM_WORLD, job.getJobTree().getLeftChildNodeRank(), MSG_NOTIFY_JOB_REVISION, payload);
    }
    if (job.getJobTree().hasRightChild()) {
        IntVec payload({jobId, revision});
        MyMpi::isend(MPI_COMM_WORLD, job.getJobTree().getRightChildNodeRank(), MSG_NOTIFY_JOB_REVISION, payload);
    }
}

void Worker::handleSendJobRevisionDetails(MessageHandle& handle) {
    IntVec response = Serializable::get<IntVec>(handle.recvData);
    int transferSize = response[3];
    MyMpi::irecv(MPI_COMM_WORLD, handle.source, MSG_SEND_JOB_REVISION_DATA, transferSize);
    MyMpi::isend(MPI_COMM_WORLD, handle.source, MSG_CONFIRM_JOB_REVISION_DETAILS, handle.recvData);
}

void Worker::handleNotifyJobTerminating(MessageHandle& handle) {
    interruptJob(Serializable::get<int>(handle.recvData), /*terminate=*/true, /*reckless=*/false);
}

void Worker::handleNotifyVolumeUpdate(MessageHandle& handle) {
    IntPair recv = Serializable::get<IntPair>(handle.recvData);
    int jobId = recv.first;
    int volume = recv.second;
    if (!_job_db.has(jobId)) {
        Console::log(Console::WARN, "[WARN] Volume update for unknown #%i", jobId);
        return;
    }

    // Update volume assignment in job instance (and its children)
    updateVolume(jobId, volume);
}

void Worker::handleNotifyNodeLeavingJob(MessageHandle& handle) {

    // Retrieve job
    IntPair recv = Serializable::get<IntPair>(handle.recvData);
    int jobId = recv.first;
    int index = recv.second;
    if (!_job_db.has(jobId)) return;
    Job& job = _job_db.get(jobId);

    // Prune away the respective child if necessary
    auto pruned = job.getJobTree().prune(handle.source, index);

    // If necessary, find replacement
    if (pruned != JobTree::TreeRelative::NONE && index < job.getVolume()) {

        // Try to find a dormant child that is not the message source
        int tag = MSG_REQUEST_NODE_ONESHOT;
        int nextNodeRank = job.getJobTree().findDormantChild(handle.source);
        if (nextNodeRank == -1) {
            // If unsucessful, pick a random node
            tag = MSG_REQUEST_NODE;
            if (nextNodeRank == -1 && _params.isNotNull("derandomize")) {
                nextNodeRank = Random::choice(_hop_destinations);
            } else if (nextNodeRank == -1) {
                nextNodeRank = getRandomNonSelfWorkerNode();
            }
        }
        
        // Initiate search for a replacement for the defected child
        JobRequest req = job.getJobTree().getJobRequestFor(jobId, pruned);
        Console::log_send(Console::VVERB, nextNodeRank, "%s : try to find child replacing defected %s", 
                        job.toStr(), _job_db.toStr(jobId, index).c_str());
        MyMpi::isend(MPI_COMM_WORLD, nextNodeRank, tag, req);
    }

    // Initiate communication if the job now became willing to communicate
    if (job.wantsToCommunicate()) job.communicate();
}

void Worker::handleNotifyResultFound(MessageHandle& handle) {

    // Retrieve job
    IntVec res = Serializable::get<IntVec>(handle.recvData);
    int jobId = res[0];
    int revision = res[1];
    if (!_job_db.has(jobId) || !_job_db.get(jobId).getJobTree().isRoot()) {
        Console::log(Console::WARN, "[WARN] Invalid adressee for job result of #%i", jobId);
        MyMpi::isend(MPI_COMM_WORLD, handle.source, MSG_NOTIFY_RESULT_OBSOLETE, handle.recvData);
        return;
    }
    if (_job_db.get(jobId).getState() == PAST) {
        Console::log_recv(Console::VVERB, handle.source, "Discard obsolete result for job #%i", jobId);
        MyMpi::isend(MPI_COMM_WORLD, handle.source, MSG_NOTIFY_RESULT_OBSOLETE, handle.recvData);
        return;
    }
    if (_job_db.get(jobId).getRevision() > revision) {
        Console::log_recv(Console::VVERB, handle.source, "Discard obsolete result for job #%i rev. %i", jobId, revision);
        MyMpi::isend(MPI_COMM_WORLD, handle.source, MSG_NOTIFY_RESULT_OBSOLETE, handle.recvData);
        return;
    }
    
    Console::log_recv(Console::VERB, handle.source, "Result found for job #%i", jobId);

    // Terminate job and propagate termination message
    if (_job_db.get(jobId).getDescription().isIncremental()) {
        handleInterrupt(handle);
    } else {
        handleNotifyJobTerminating(handle);
    }

    // Redirect termination signal
    IntPair payload(jobId, _job_db.get(jobId).getJobTree().getParentNodeRank());
    if (handle.source == _world_rank) {
        // Self-message of root node: Directly send termination message to client
        informClientJobIsDone(payload.first, payload.second);
    } else {
        // Send rank of client node to the finished worker,
        // such that the worker can inform the client of the result
        MyMpi::isend(MPI_COMM_WORLD, handle.source, MSG_SEND_CLIENT_RANK, payload);
        Console::log_send(Console::VVERB, handle.source, "Forward rank of client (%i)", payload.second); 
    }
}

void Worker::bounceJobRequest(JobRequest& request, int senderRank) {

    // Increment #hops
    request.numHops++;
    int num = request.numHops;
    _sys_state.addLocal(SYSSTATE_NUMHOPS, 1);

    // Show warning if #hops is a large power of two
    if ((num >= 512) && ((num & (num - 1)) == 0)) {
        Console::log(Console::WARN, "[WARN] Hop no. %i for %s", num, _job_db.toStr(request.jobId, request.requestedNodeIndex).c_str());
    }

    int nextRank;
    if (_params.isNotNull("derandomize")) {
        // Get random choice from bounce alternatives
        nextRank = Random::choice(_hop_destinations);
        if (_hop_destinations.size() > 2) {
            // ... if possible while skipping the requesting node and the sender
            while (nextRank == request.requestingNodeRank || nextRank == senderRank) {
                nextRank = Random::choice(_hop_destinations);
            }
        }
    } else {
        // Generate pseudorandom permutation of this request
        int n = MyMpi::size(_comm);
        AdjustablePermutation perm(n, 3 * request.jobId + 7 * request.requestedNodeIndex + 11 * request.requestingNodeRank);
        // Fetch next index of permutation based on number of hops
        int permIdx = request.numHops % n;
        nextRank = perm.get(permIdx);
        if (n > 3) {
            // ... if possible while skipping yourself, the requesting node, and the sender
            while (nextRank == _world_rank || nextRank == request.requestingNodeRank || nextRank == senderRank) {
                permIdx = (permIdx+1) % n;
                nextRank = perm.get(permIdx);
            }
        }
    }

    // Send request to "next" worker node
    Console::log_send(Console::VVVERB, nextRank, "Hop %s", _job_db.toStr(request.jobId, request.requestedNodeIndex).c_str());
    MyMpi::isend(MPI_COMM_WORLD, nextRank, MSG_REQUEST_NODE, request);
}

void Worker::updateVolume(int jobId, int volume) {

    if (!_job_db.has(jobId)) return;
    Job &job = _job_db.get(jobId);

    if (job.getState() != ACTIVE) {
        // Job is not active right now
        return;
    }

    // Root node update message
    int thisIndex = job.getIndex();
    if (thisIndex == 0 && job.getVolume() != volume) {
        Console::log(Console::VERB, "%s : update v=%i", job.toStr(), volume);
    }
    job.updateVolumeAndUsedCpu(volume);

    // Prepare volume update to propagate down the job tree
    IntPair payload(jobId, volume);

    // Mono instance mode: Set job tree permutation to identity
    bool mono = _params.isNotNull("mono");

    // For each potential child (left, right):
    std::set<int> dormantChildren = job.getJobTree().getDormantChildren();
    bool has[2] = {job.getJobTree().hasLeftChild(), job.getJobTree().hasRightChild()};
    int indices[2] = {job.getJobTree().getLeftChildIndex(), job.getJobTree().getRightChildIndex()};
    int ranks[2] = {-1, -1};
    for (int i = 0; i < 2; i++) {
        int nextIndex = indices[i];
        if (has[i]) {
            ranks[i] = i == 0 ? job.getJobTree().getLeftChildNodeRank() : job.getJobTree().getRightChildNodeRank();
            // Propagate volume update
            MyMpi::isend(MPI_COMM_WORLD, ranks[i], MSG_NOTIFY_VOLUME_UPDATE, payload);

        } else if ((job.isReadyToGrow() || job.testReadyToGrow()) && nextIndex < volume) {
            // Grow
            Console::log(Console::VVVERB, "%s : grow", job.toStr());
            if (mono) job.getJobTree().updateJobNode(indices[i], indices[i]);
            JobRequest req(jobId, job.getJobTree().getRootNodeRank(), _world_rank, nextIndex, Timer::elapsedSeconds(), 0);
            int nextNodeRank, tag;
            if (dormantChildren.empty()) {
                tag = MSG_REQUEST_NODE;
                ranks[i] = i == 0 ? job.getJobTree().getLeftChildNodeRank() : job.getJobTree().getRightChildNodeRank();
                nextNodeRank = ranks[i];
            } else {
                tag = MSG_REQUEST_NODE_ONESHOT;
                nextNodeRank = Random::choice(dormantChildren);
                dormantChildren.erase(nextNodeRank);
            }
            Console::log_send(Console::VERB, nextNodeRank, "%s growing, request %s", 
                        job.toStr(), _job_db.toStr(job.getId(), nextIndex).c_str());
            MyMpi::isend(MPI_COMM_WORLD, nextNodeRank, tag, req);
        }
    }

    // Shrink (and pause solving) if necessary
    if (thisIndex > 0 && thisIndex >= volume) {
        _job_db.suspend(jobId);
        MyMpi::isend(MPI_COMM_WORLD, job.getJobTree().getParentNodeRank(), MSG_NOTIFY_NODE_LEAVING_JOB, IntPair(jobId, thisIndex));
    }
}

void Worker::interruptJob(int jobId, bool terminate, bool reckless) {

    if (!_job_db.has(jobId)) return;
    Job& job = _job_db.get(jobId);

    // Propagate message down the job tree
    int msgTag;
    if (terminate && reckless) msgTag = MSG_NOTIFY_JOB_ABORTING;
    else if (terminate) msgTag = MSG_NOTIFY_JOB_TERMINATING;
    else msgTag = MSG_INTERRUPT;
    if (job.getJobTree().hasLeftChild()) {
        MyMpi::isend(MPI_COMM_WORLD, job.getJobTree().getLeftChildNodeRank(), msgTag, IntVec({jobId}));
        Console::log_send(Console::VVERB, job.getJobTree().getLeftChildNodeRank(), "Propagate interruption of %s ...", job.toStr());
    }
    if (job.getJobTree().hasRightChild()) {
        MyMpi::isend(MPI_COMM_WORLD, job.getJobTree().getRightChildNodeRank(), msgTag, IntVec({jobId}));
        Console::log_send(Console::VVERB, job.getJobTree().getRightChildNodeRank(), "Propagate interruption of %s ...", job.toStr());
    }
    for (auto childRank : job.getJobTree().getPastChildren()) {
        MyMpi::isend(MPI_COMM_WORLD, childRank, msgTag, IntVec({jobId}));
        Console::log_send(Console::VVERB, childRank, "Propagate interruption of %s (past child) ...", job.toStr());
    }
    job.getJobTree().getPastChildren().clear();

    // Stop, and possibly terminate, the job
    _job_db.stop(jobId, terminate);
}

void Worker::informClientJobIsDone(int jobId, int clientRank) {
    const JobResult& result = _job_db.get(jobId).getResult();

    // Send "Job done!" with advertised result size to client
    Console::log_send(Console::VVERB, clientRank, "%s : send JOB_DONE to client", _job_db.get(jobId).toStr());
    IntPair payload(jobId, result.getTransferSize());
    MyMpi::isend(MPI_COMM_WORLD, clientRank, MSG_NOTIFY_JOB_DONE, payload);
}

void Worker::timeoutJob(int jobId) {
    // "Virtual self message" aborting the job
    IntVec payload({jobId});
    MessageHandle handle(MyMpi::nextHandleId());
    handle.source = _world_rank;
    handle.recvData = payload.serialize();
    handle.tag = MSG_NOTIFY_JOB_ABORTING;
    handle.finished = true;
    handleNotifyJobAborting(handle);
}

void Worker::applyBalancing() {
    
    // Update volumes found during balancing, and trigger job expansions / shrinkings
    for (const auto& [jobId, volume] : _job_db.getBalancingResult()) {
        /*
        if (_job_db.has(jobId) && _job_db.get(jobId).getVolume() != volume) {
            Console::log(
                _job_db.get(jobId).getJobTree().isRoot() ? Console::INFO : Console::VVERB, 
                "#%i : update v=%i", jobId, volume
            );
        }
        */
        updateVolume(jobId, volume);
    }
}

bool Worker::checkTerminate(float time) {
    bool terminate = false;
    if (_exiting) terminate = true;
    if (Process::isExitSignalCaught()) terminate = true;
    if (_global_timeout > 0 && time > _global_timeout) {
        terminate = true;
    }
    if (terminate) {
        Console::log(_world_rank == 0 ? Console::INFO : Console::VERB, 
                "Terminating.");
        return true;
    }
    return false;
}

void Worker::createExpanderGraph() {

    // Pick fixed number k of bounce destinations
    int numBounceAlternatives = _params.getIntParam("ba");
    int numWorkers = MyMpi::size(_comm);

    // Check validity of num bounce alternatives
    if (2*numBounceAlternatives > numWorkers) {
        numBounceAlternatives = numWorkers / 2;
        Console::log(Console::WARN, "[WARN] Num bounce alternatives must be at most half the number of workers!");
        Console::log(Console::WARN, "[WARN] Falling back to safe value r=%i.", numBounceAlternatives);
    }  

    // Create graph, get outgoing edges from this node
    _hop_destinations = AdjustablePermutation::createExpanderGraph(numWorkers, numBounceAlternatives, _world_rank);
    assert((int)_hop_destinations.size() == numBounceAlternatives);

    // Output found bounce alternatives
    std::string info = "";
    for (size_t i = 0; i < _hop_destinations.size(); i++) {
        info += std::to_string(_hop_destinations[i]) + " ";
    }
    Console::log(Console::VERB, "My bounce alternatives: %s", info.c_str());
}

int Worker::getRandomNonSelfWorkerNode() {

    // All clients are excluded from drawing
    std::set<int> excludedNodes = std::set<int>(this->_client_nodes);
    // THIS node is also excluded from drawing
    excludedNodes.insert(_world_rank);
    // Draw a node from the remaining nodes
    int size = MyMpi::size(_comm);

    float r = Random::rand();
    int node = (int) (r * size);
    while (excludedNodes.find(node) != excludedNodes.end()) {
        r = Random::rand();
        node = (int) (r * size);
    }

    return node;
}

Worker::~Worker() {

    _exiting = true;

    // Send termination signal to the entire process group 
    Process::terminateAll();
    
    // -- quicker than normal terminate
    // -- workaround for idle times after finishing
    //raise(SIGTERM);

    if (_mpi_monitor_thread.joinable())
        _mpi_monitor_thread.join();

    Console::log(Console::VVERB, "Destruct worker");
}