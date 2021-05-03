
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
#include "util/logger.hpp"
#include "util/random.hpp"
#include "util/sat_reader.hpp"
#include "util/sys/terminator.hpp"

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
        log(LOG_ADD_SRCRANK | V5_DEBG, "Warmup msg", h.source);
    });
    _msg_handler.registerCallback(MessageHandler::TAG_DEFAULT, [&](auto& h) {
        log(LOG_ADD_SRCRANK | V1_WARN, "[WARN] Unknown message tag %i", h.source, h.tag);
    });
    MyMpi::beginListening();

    // Send warm-up messages with your pseudorandom bounce destinations
    if (_params.isNotNull("derandomize") && _params.isNotNull("warmup")) {
        IntVec payload({1, 2, 3, 4, 5, 6, 7, 8});
        int numRuns = 5;
        for (int run = 0; run < numRuns; run++) {
            for (auto rank : _hop_destinations) {
                MyMpi::isend(MPI_COMM_WORLD, rank, MSG_WARMUP, payload);
                log(LOG_ADD_DESTRANK | V4_VVER, "Warmup msg", rank);
            }
        }
    }

    log(V4_VVER, "Global init barrier ...\n");
    MPI_Barrier(MPI_COMM_WORLD);
    log(V4_VVER, "Passed global init barrier\n");

    if (!MyMpi::_monitor_off) _mpi_monitor_thread = std::thread(mpiMonitor);
    
    // Initiate single instance solving as the "root node"
    if (_params.isNotNull("mono") && _world_rank == 0) {

        std::string instanceFilename = _params.getParam("mono");
        log(V2_INFO, "Initiate solving of mono instance \"%s\"\n", instanceFilename.c_str());

        // Create job description with formula
        log(V3_VERB, "read instance\n");
        int jobId = 1;
        JobDescription desc(jobId, /*prio=*/1, /*incremental=*/false);
        desc.setRootRank(0);
        bool success = SatReader(instanceFilename).read(desc);
        if (!success) {
            log(V0_CRIT, "Could not open file! Aborting.\n");
            Terminator::setTerminating();
            return;
        }
        log(V3_VERB, "%ld lits w/ separators; lits = %i %i %i %i ...\n", desc.getFormulaSize(),
                desc.getFormulaPayload()[0], desc.getFormulaPayload()[1], desc.getFormulaPayload()[2], desc.getFormulaPayload()[3]);

        // Add as a new local SAT job image
        log(V3_VERB, "%ld lits w/ separators; init SAT job image\n", desc.getFormulaSize());
        _job_db.createJob(MyMpi::size(_comm), _world_rank, jobId);
        JobRequest req(jobId, 0, 0, 0, 0, 0);
        _job_db.commit(req);
        auto serializedDesc = desc.getSerialization();
        initJob(jobId, serializedDesc, _world_rank);
    }
}

void Worker::mainProgram() {

    int iteration = 0;
    float lastMemCheckTime = Timer::elapsedSeconds();
    float lastJobCheckTime = lastMemCheckTime;
    float lastBalanceCheckTime = lastMemCheckTime;
    float lastMaintenanceCheckTime = lastMemCheckTime;

    float sleepMicrosecs = _params.getFloatParam("sleep");
    float jobCheckPeriod = 0.01;
    float balanceCheckPeriod = 0.01;
    float maintenanceCheckPeriod = 1.0;
    float memCheckPeriod = 3.0;
    bool doYield = _params.isNotNull("yield");

    Watchdog watchdog(/*checkIntervMillis=*/200, lastMemCheckTime);
    watchdog.setWarningPeriod(100); // warn after 0.1s without a reset
    watchdog.setAbortPeriod(60*1000); // abort after 60s without a reset

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
            log(V4_VVER, "mainthread_cpu=%i\n", info.cpu);
            log(V3_VERB, "mem=%.2fGB\n", info.residentSetSize);
            _sys_state.setLocal(SYSSTATE_GLOBALMEM, info.residentSetSize);

            // For this "management" thread
            double cpuShare; float sysShare;
            bool success = Proc::getThreadCpuRatio(Proc::getTid(), cpuShare, sysShare);
            if (success) {
                log(V3_VERB, "mainthread cpuratio=%.3f sys=%.3f\n", cpuShare, sysShare);
            }

            // For the current job
            if (!_job_db.isIdle()) {
                Job& job = _job_db.getActive();
                job.appl_dumpStats();
                std::string commStr = "";
                for (size_t i = 0; i < job.getJobComm().size(); i++) {
                    commStr += " " + std::to_string(job.getJobComm()[i]);
                }
                log(V4_VVER, "%s job comm:%s\n", job.toStr(), commStr.c_str());
            }
        }

        // Advance load balancing operations
        if (time - lastBalanceCheckTime > balanceCheckPeriod) {
            lastBalanceCheckTime = time;
            if (_job_db.isTimeForRebalancing()) {
                if (_job_db.beginBalancing()) applyBalancing();
            } 
            if (_job_db.continueBalancing()) applyBalancing();

            // Reset watchdog
            watchdog.reset(time);
        }

        // Do diverse periodic maintenance tasks
        if (time - lastMaintenanceCheckTime > maintenanceCheckPeriod) {
            lastMaintenanceCheckTime = time;

            // Forget jobs that are old or wasting memory
            _job_db.forgetOldJobs();

            // Continue to bounce requests which were deferred earlier
            for (auto& [req, senderRank] : _job_db.getDeferredRequestsToForward(time)) {
                bounceJobRequest(req, senderRank);
            }
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
                    
                    // Check if a result was found
                    int result = job.appl_solved();
                    if (result >= 0) {
                        // Solver done!
                        // Signal termination to root -- may be a self message
                        int jobRootRank = job.getJobTree().getRootNodeRank();
                        IntVec payload({job.getId(), job.getRevision(), result});
                        log(LOG_ADD_DESTRANK | V4_VVER, "%s : sending finished info", jobRootRank, job.toStr());
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
            int verb = (_world_rank == 0 ? V2_INFO : V5_DEBG);
            log(verb, "sysstate busyratio=%.3f jobs=%i globmem=%.2fGB newreqs=%i hops=%i\n", 
                        result[0]/MyMpi::size(_comm), (int)result[1], result[2], (int)result[4], (int)result[3]);
            _sys_state.setLocal(SYSSTATE_NUMHOPS, 0); // reset #hops
            _sys_state.setLocal(SYSSTATE_SPAWNEDREQUESTS, 0); // reset #requests
        }

        if (sleepMicrosecs > 0) usleep(sleepMicrosecs);
        if (doYield) std::this_thread::yield();

        time = Timer::elapsedSeconds();
    }

    watchdog.stop();
    Logger::getMainInstance().flush();
    fflush(stdout);
}

void Worker::handleNotifyJobAborting(MessageHandle& handle) {

    int jobId = Serializable::get<int>(handle.getRecvData());
    if (!_job_db.has(jobId)) return;

    interruptJob(jobId, /*terminate=*/true, /*reckless=*/true);
    
    if (!_params.isNotNull("mono") && _job_db.get(jobId).getJobTree().isRoot()) {
        // Forward information on aborted job to client
        MyMpi::isend(MPI_COMM_WORLD, _job_db.get(jobId).getJobTree().getParentNodeRank(), 
            MSG_NOTIFY_JOB_ABORTING, handle.getRecvData());
    }
}

void Worker::handleAcceptAdoptionOffer(MessageHandle& handle) {

    // Retrieve according job commitment
    JobSignature sig = Serializable::get<JobSignature>(handle.getRecvData());
    if (!_job_db.hasCommitment(sig.jobId)) {
        log(V1_WARN, "[WARN] Job commitment for #%i not present despite adoption accept msg\n", sig.jobId);
        return;
    }

    const JobRequest& req = _job_db.getCommitment(sig.jobId);

    if (req.fullTransfer == 1) {
        // Full transfer of job description is required:
        // Send ACK to parent and receive full job description
        log(V4_VVER, "Will receive desc. of #%i, size %i\n", req.jobId, sig.getTransferSize());
        MyMpi::isend(MPI_COMM_WORLD, handle.source, MSG_CONFIRM_ADOPTION, req);
        MyMpi::irecv(MPI_COMM_WORLD, handle.source, MSG_SEND_JOB_DESCRIPTION, sig.getTransferSize()); // to be received later
    } else {
        _job_db.uncommit(req.jobId);
        _job_db.reactivate(req, handle.source);
        // Query parent for current volume of job
        MyMpi::isend(MPI_COMM_WORLD, handle.source, MSG_QUERY_VOLUME, IntVec({req.jobId}));
    }
}

void Worker::handleConfirmJobRevisionDetails(MessageHandle& handle) {
    IntVec response = Serializable::get<IntVec>(handle.getRecvData());
    int jobId = response[0];
    int firstRevision = response[1];
    int lastRevision = response[2];

    // TODO not implemented.
    abort();
}

void Worker::handleConfirmAdoption(MessageHandle& handle) {
    JobRequest req = Serializable::get<JobRequest>(handle.getRecvData());

    // If job offer is obsolete, send a stub description containing the job id ONLY
    if (_job_db.isAdoptionOfferObsolete(req, /*alreadyAccepted=*/true)) {
        // Obsolete request
        log(LOG_ADD_SRCRANK | V3_VERB, "REJECT %s", handle.source, req.toStr().c_str());
        MyMpi::isend(MPI_COMM_WORLD, handle.source, MSG_SEND_JOB_DESCRIPTION, IntVec({req.jobId}));
        return;
    }
    Job& job = _job_db.get(req.jobId);

    // Retrieve and send concerned job description
    const auto& descPtr = job.getSerializedDescription();
    MyMpi::isend(MPI_COMM_WORLD, handle.source, MSG_SEND_JOB_DESCRIPTION, descPtr);
    log(LOG_ADD_DESTRANK | V4_VVER, "Sent job desc. of %s, size %i", handle.source, 
            job.toStr(), descPtr->size());

    // Mark new node as one of the node's children
    auto relative = job.getJobTree().setChild(handle.source, req.requestedNodeIndex);
    if (relative == JobTree::TreeRelative::NONE) assert(req.requestedNodeIndex == 0);
}

void Worker::handleDoExit(MessageHandle& handle) {
    log(LOG_ADD_SRCRANK | V3_VERB, "Received exit signal", handle.source);

    // Forward exit signal
    if (_world_rank*2+1 < MyMpi::size(MPI_COMM_WORLD))
        MyMpi::isend(MPI_COMM_WORLD, _world_rank*2+1, MSG_DO_EXIT, handle.getRecvData());
    if (_world_rank*2+2 < MyMpi::size(MPI_COMM_WORLD))
        MyMpi::isend(MPI_COMM_WORLD, _world_rank*2+2, MSG_DO_EXIT, handle.getRecvData());
    MyMpi::testSentHandles();

    Terminator::setTerminating();
}

void Worker::handleRejectOneshot(MessageHandle& handle) {
    JobRequest req = Serializable::get<JobRequest>(handle.getRecvData());
    log(LOG_ADD_SRCRANK | V5_DEBG, "%s rejected by dormant child", handle.source, 
            _job_db.toStr(req.jobId, req.requestedNodeIndex).c_str());

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
            log(LOG_ADD_DESTRANK | V4_VVER, "%s : query dormant child", rank, job.toStr());
            _sys_state.addLocal(SYSSTATE_SPAWNEDREQUESTS, 1);
        }
    }

    if (doNormalHopping) {
        log(V4_VVER, "%s : switch to normal hops\n", job.toStr());
        req.numHops = -1;
        bounceJobRequest(req, handle.source);
    }
}

void Worker::handleRequestNode(MessageHandle& handle, bool oneshot) {

    JobRequest req = Serializable::get<JobRequest>(handle.getRecvData());

    // Discard request if it has become obsolete
    if (_job_db.isRequestObsolete(req)) {
        log(LOG_ADD_SRCRANK | V3_VERB, "DISCARD %s oneshot=%i", handle.source, 
                req.toStr().c_str(), oneshot ? 1 : 0);
        return;
    }

    int removedJob;
    auto adoptionResult = _job_db.tryAdopt(req, oneshot, handle.source, removedJob);
    if (adoptionResult == JobDatabase::ADOPT_FROM_IDLE || adoptionResult == JobDatabase::ADOPT_REPLACE_CURRENT) {

        if (adoptionResult == JobDatabase::ADOPT_REPLACE_CURRENT) {
            Job& job = _job_db.get(removedJob);
            IntPair pair(job.getId(), job.getIndex());
            MyMpi::isend(MPI_COMM_WORLD, job.getJobTree().getParentNodeRank(), MSG_NOTIFY_NODE_LEAVING_JOB, pair);
        }

        // Adoption takes place
        std::string jobstr = _job_db.toStr(req.jobId, req.requestedNodeIndex);
        log(LOG_ADD_SRCRANK | V3_VERB, "ADOPT %s oneshot=%i", handle.source, req.toStr().c_str(), oneshot ? 1 : 0);
        assert(_job_db.isIdle() || log_return_false("Adopting a job, but not idle!\n"));

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

    } else if (adoptionResult == JobDatabase::REJECT) {
        if (oneshot) {
            log(LOG_ADD_DESTRANK | V5_DEBG, "decline oneshot request for %s", handle.source, 
                        _job_db.toStr(req.jobId, req.requestedNodeIndex).c_str());
            MyMpi::isend(MPI_COMM_WORLD, handle.source, MSG_REJECT_ONESHOT, handle.getRecvData());
        } else {
            // Continue job finding procedure somewhere else
            bounceJobRequest(req, handle.source);
        }
    }
}

void Worker::handleSendClientRank(MessageHandle& handle) {

    // Receive rank of the job's client
    IntPair recv = Serializable::get<IntPair>(handle.getRecvData());
    int jobId = recv.first;
    int clientRank = recv.second;
    assert(_job_db.has(jobId));

    // Inform client of the found job result
    informClientJobIsDone(jobId, clientRank);
}

void Worker::handleIncrementalJobFinished(MessageHandle& handle) {
    interruptJob(Serializable::get<int>(handle.getRecvData()), /*terminate=*/true, /*reckless=*/false);
}

void Worker::handleInterrupt(MessageHandle& handle) {
    interruptJob(Serializable::get<int>(handle.getRecvData()), /*terminate=*/false, /*reckless=*/false);
}

void Worker::handleSendApplicationMessage(MessageHandle& handle) {

    // Deserialize job-specific message
    JobMessage msg = Serializable::get<JobMessage>(handle.getRecvData());
    int jobId = msg.jobId;
    if (!_job_db.has(jobId)) {
        log(V1_WARN, "[WARN] Job message from unknown job #%i\n", jobId);
        return;
    }
    // Give message to corresponding job
    Job& job = _job_db.get(jobId);
    if (job.getState() == ACTIVE) job.communicate(handle.source, msg);
}

void Worker::handleNotifyJobDone(MessageHandle& handle) {
    IntPair recv = Serializable::get<IntPair>(handle.getRecvData());
    int jobId = recv.first;
    int resultSize = recv.second;
    log(LOG_ADD_SRCRANK | V4_VVER, "Will receive job result, length %i, for job #%i", handle.source, resultSize, jobId);
    MyMpi::irecv(MPI_COMM_WORLD, handle.source, MSG_SEND_JOB_RESULT, resultSize);
    MyMpi::isend(MPI_COMM_WORLD, handle.source, MSG_QUERY_JOB_RESULT, handle.getRecvData());
}

void Worker::handleNotifyJobRevision(MessageHandle& handle) {
    IntVec payload = Serializable::get<IntVec>(handle.getRecvData());
    int jobId = payload[0];
    int revision = payload[1];

    assert(_job_db.has(jobId));

    // Request receiving information on revision size
    Job& job = _job_db.get(jobId);
    int lastKnownRevision = job.getRevision();
    if (revision > lastKnownRevision) {
        log(V3_VERB, "Received revision update #%i rev. %i (I am at rev. %i)\n", jobId, revision, lastKnownRevision);
        IntVec request({jobId, lastKnownRevision+1, revision});
        MyMpi::isend(MPI_COMM_WORLD, handle.source, MSG_QUERY_JOB_REVISION_DETAILS, request);
    } else {
        // TODO ???
        log(V1_WARN, "[WARN] Useless revision update #%i rev. %i (I am already at rev. %i)\n", jobId, revision, lastKnownRevision);
    }
}

void Worker::handleOfferAdoption(MessageHandle& handle) {

    JobRequest req = Serializable::get<JobRequest>(handle.getRecvData());
    log(LOG_ADD_SRCRANK | V4_VVER, "Adoption offer for %s", handle.source, 
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
            log(LOG_ADD_SRCRANK | V3_VERB, "REJECT %s", handle.source, req.toStr().c_str());
            reject = true;

        } else {
            // Adopt the job

            // Send job signature
            JobSignature sig(req.jobId, req.rootRank, req.revision, job.getSerializedDescription()->size());
            MyMpi::isend(MPI_COMM_WORLD, handle.source, MSG_ACCEPT_ADOPTION_OFFER, sig);

            // If req.fullTransfer, then wait for the child to acknowledge having received the signature
            if (req.fullTransfer == 1) {
                log(LOG_ADD_DESTRANK | V4_VVER, "Will send desc. of %s", handle.source, 
                        _job_db.toStr(req.jobId, req.requestedNodeIndex).c_str());
            } else {
                log(LOG_ADD_DESTRANK | V4_VVER, "Resume child %s", handle.source, 
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
    int jobId = Serializable::get<int>(handle.getRecvData());
    assert(_job_db.has(jobId));
    const JobResult& result = _job_db.get(jobId).getResult();
    log(LOG_ADD_DESTRANK | V3_VERB, "Send full result to client", handle.source);
    MyMpi::isend(MPI_COMM_WORLD, handle.source, MSG_SEND_JOB_RESULT, result);
    _job_db.get(jobId).setResultTransferPending(false);
}

void Worker::handleQueryJobRevisionDetails(MessageHandle& handle) {
    IntVec request = Serializable::get<IntVec>(handle.getRecvData());
    int jobId = request[0];
    int firstRevision = request[1];
    int lastRevision = request[2];

    assert(_job_db.has(jobId));

    const JobDescription& desc = _job_db.get(jobId).getDescription();
    IntVec response({jobId, firstRevision, lastRevision, desc.getFullTransferSize()});
    MyMpi::isend(MPI_COMM_WORLD, handle.source, MSG_SEND_JOB_REVISION_DETAILS, response);
}

void Worker::handleQueryVolume(MessageHandle& handle) {

    IntVec payload = Serializable::get<IntVec>(handle.getRecvData());
    int jobId = payload[0];

    // Unknown job? -- ignore.
    if (!_job_db.has(jobId)) return;

    Job& job = _job_db.get(jobId);
    int volume = job.getVolume();
    
    // Volume is unknown right now? Query parent recursively. 
    // (Answer will flood back to the entire subtree)
    if (job.getState() == ACTIVE && volume == 0) {
        assert(!job.getJobTree().isRoot());
        MyMpi::isend(MPI_COMM_WORLD, job.getJobTree().getParentNodeRank(), MSG_QUERY_VOLUME, handle.getRecvData());
        return;
    }

    // Send response
    IntVec response({jobId, volume});
    log(LOG_ADD_DESTRANK | V4_VVER, "Answer #%i volume query with v=%i", handle.source, jobId, volume);
    MyMpi::isend(MPI_COMM_WORLD, handle.source, MSG_NOTIFY_VOLUME_UPDATE, response);
}

void Worker::handleRejectAdoptionOffer(MessageHandle& handle) {

    // Retrieve committed job
    JobRequest req = Serializable::get<JobRequest>(handle.getRecvData());
    if (!_job_db.has(req.jobId)) return;
    Job &job = _job_db.get(req.jobId);

    // Erase commitment
    log(LOG_ADD_SRCRANK | V4_VVER, "Rejected to become %s : uncommitting", handle.source, job.toStr());
    _job_db.uncommit(req.jobId);
}

void Worker::handleNotifyResultObsolete(MessageHandle& handle) {
    IntVec res = Serializable::get<IntVec>(handle.getRecvData());
    int jobId = res[0];
    //int revision = res[1];
    if (!_job_db.has(jobId)) return;
    log(LOG_ADD_SRCRANK | V4_VVER, "job result for %s unwanted", handle.source, _job_db.get(jobId).toStr());
    _job_db.get(jobId).setResultTransferPending(false);
}

void Worker::handleSendJob(MessageHandle& handle) {
    const auto& data = handle.getRecvData();
    int jobId = data.size() >= sizeof(int) ? Serializable::get<int>(data) : -1;
    log(LOG_ADD_SRCRANK | V4_VVER, "Got desc. of size %i for job #%i", handle.source, data.size(), jobId);
    initJob(jobId, std::shared_ptr<std::vector<uint8_t>>(
        new std::vector<uint8_t>(handle.moveRecvData())
    ), handle.source);
}

void Worker::initJob(int jobId, const std::shared_ptr<std::vector<uint8_t>>& data, int senderRank) {

    bool success = _job_db.init(jobId, data, senderRank);
    if (success) {
        auto& job = _job_db.get(jobId);
        if (job.getJobTree().isRoot()) {
            // Root worker: update volume (to trigger growth if desired)
            if (job.getVolume() > 1) updateVolume(jobId, job.getVolume());
        } else {
            // Non-root worker: query parent for the volume of this job
            IntVec payload({jobId});
            MyMpi::isend(MPI_COMM_WORLD, job.getJobTree().getParentNodeRank(), MSG_QUERY_VOLUME, payload);
        }
    }
}

void Worker::handleSendJobResult(MessageHandle& handle) {
    JobResult jobResult = Serializable::get<JobResult>(handle.getRecvData());
    int jobId = jobResult.id;
    int resultCode = jobResult.result;
    int revision = jobResult.revision;

    log(LOG_ADD_SRCRANK | V2_INFO, "Received result of job #%i rev. %i, code: %i", handle.source, jobId, revision, resultCode);
    std::string resultString = "s " + std::string(resultCode == RESULT_SAT ? "SATISFIABLE" 
                        : resultCode == RESULT_UNSAT ? "UNSATISFIABLE" : "UNKNOWN") + "\n";
    std::stringstream modelString;
    if (resultCode == RESULT_SAT) {
        modelString << "v ";
        for (size_t x = 1; x < jobResult.solution.size(); x++) {
            modelString << std::to_string(jobResult.solution[x]) << " ";
        }
        modelString << "0\n";
    }
    if (_params.isNotNull("s2f")) {
        std::ofstream file;
        file.open(_params.getParam("s2f"));
        if (!file.is_open()) {
            log(V0_CRIT, "ERROR: Could not open solution file\n");
        } else {
            file << resultString;
            file << modelString.str();
            file.close();
        }
    } else {
        log(LOG_NO_PREFIX | V0_CRIT, resultString.c_str());
        log(LOG_NO_PREFIX | V0_CRIT, modelString.str().c_str());
    }

    if (_params.isNotNull("mono")) {
        // Single instance solving is done: begin exit signal
        MyMpi::isend(MPI_COMM_WORLD, 0, MSG_DO_EXIT, IntVec({0}));
    }
}

void Worker::handleSendJobRevisionData(MessageHandle& handle) {
    int jobId = Serializable::get<int>(handle.getRecvData());
    assert(_job_db.has(jobId));

    // TODO in separate thread
    Job& job = _job_db.get(jobId);
    //job.unpackAmendment(handle.getRecvData());
    int revision = job.getDescription().getRevision();
    log(V2_INFO, "%s : computing on #%i rev. %i\n", job.toStr(), jobId, revision);
    
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
    IntVec response = Serializable::get<IntVec>(handle.getRecvData());
    int transferSize = response[3];
    MyMpi::irecv(MPI_COMM_WORLD, handle.source, MSG_SEND_JOB_REVISION_DATA, transferSize);
    MyMpi::isend(MPI_COMM_WORLD, handle.source, MSG_CONFIRM_JOB_REVISION_DETAILS, handle.getRecvData());
}

void Worker::handleNotifyJobTerminating(MessageHandle& handle) {
    interruptJob(Serializable::get<int>(handle.getRecvData()), /*terminate=*/true, /*reckless=*/false);
}

void Worker::handleNotifyVolumeUpdate(MessageHandle& handle) {
    IntPair recv = Serializable::get<IntPair>(handle.getRecvData());
    int jobId = recv.first;
    int volume = recv.second;
    if (!_job_db.has(jobId)) {
        log(V1_WARN, "[WARN] Volume update for unknown #%i\n", jobId);
        return;
    }

    // Update volume assignment in job instance (and its children)
    updateVolume(jobId, volume);
}

void Worker::handleNotifyNodeLeavingJob(MessageHandle& handle) {

    // Retrieve job
    IntPair recv = Serializable::get<IntPair>(handle.getRecvData());
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
            if (_params.isNotNull("derandomize")) {
                nextNodeRank = Random::choice(_hop_destinations);
            } else {
                nextNodeRank = getRandomNonSelfWorkerNode();
            }
        }
        
        // Initiate search for a replacement for the defected child
        JobRequest req = job.getJobTree().getJobRequestFor(jobId, pruned);
        log(LOG_ADD_DESTRANK | V4_VVER, "%s : try to find child replacing defected %s", nextNodeRank, 
                        job.toStr(), _job_db.toStr(jobId, index).c_str());
        MyMpi::isend(MPI_COMM_WORLD, nextNodeRank, tag, req);
        _sys_state.addLocal(SYSSTATE_SPAWNEDREQUESTS, 1);
    }

    // Initiate communication if the job now became willing to communicate
    if (job.wantsToCommunicate()) job.communicate();
}

void Worker::handleNotifyResultFound(MessageHandle& handle) {

    // Retrieve job
    IntVec res = Serializable::get<IntVec>(handle.getRecvData());
    int jobId = res[0];
    int revision = res[1];
    if (!_job_db.has(jobId) || !_job_db.get(jobId).getJobTree().isRoot()) {
        log(V1_WARN, "[WARN] Invalid adressee for job result of #%i\n", jobId);
        MyMpi::isend(MPI_COMM_WORLD, handle.source, MSG_NOTIFY_RESULT_OBSOLETE, handle.getRecvData());
        return;
    }
    if (_job_db.get(jobId).getState() == PAST) {
        log(LOG_ADD_SRCRANK | V4_VVER, "Discard obsolete result for job #%i", handle.source, jobId);
        MyMpi::isend(MPI_COMM_WORLD, handle.source, MSG_NOTIFY_RESULT_OBSOLETE, handle.getRecvData());
        return;
    }
    if (_job_db.get(jobId).getRevision() > revision) {
        log(LOG_ADD_SRCRANK | V4_VVER, "Discard obsolete result for job #%i rev. %i", handle.source, jobId, revision);
        MyMpi::isend(MPI_COMM_WORLD, handle.source, MSG_NOTIFY_RESULT_OBSOLETE, handle.getRecvData());
        return;
    }
    
    log(LOG_ADD_SRCRANK | V3_VERB, "Result found for job #%i", handle.source, jobId);

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
        log(LOG_ADD_DESTRANK | V4_VVER, "Forward rank of client (%i)", handle.source, payload.second); 
    }
}

void Worker::bounceJobRequest(JobRequest& request, int senderRank) {

    // Increment #hops
    request.numHops++;
    int num = request.numHops;
    _sys_state.addLocal(SYSSTATE_NUMHOPS, 1);

    // Show warning if #hops is a large power of two
    if ((num >= 512) && ((num & (num - 1)) == 0)) {
        log(V1_WARN, "[WARN] %s\n", request.toStr().c_str());
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
    log(LOG_ADD_DESTRANK | V5_DEBG, "Hop %s", nextRank, _job_db.toStr(request.jobId, request.requestedNodeIndex).c_str());
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
        log(V3_VERB, "%s : update v=%i\n", job.toStr(), volume);
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

        } else if (nextIndex < volume) {
            // Grow
            log(V5_DEBG, "%s : grow\n", job.toStr());
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
            log(LOG_ADD_DESTRANK | V3_VERB, "%s growing, request %s", nextNodeRank, 
                        job.toStr(), _job_db.toStr(job.getId(), nextIndex).c_str());
            MyMpi::isend(MPI_COMM_WORLD, nextNodeRank, tag, req);
            _sys_state.addLocal(SYSSTATE_SPAWNEDREQUESTS, 1);
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
        log(LOG_ADD_DESTRANK | V4_VVER, "Propagate interruption of %s ...", job.getJobTree().getLeftChildNodeRank(), job.toStr());
    }
    if (job.getJobTree().hasRightChild()) {
        MyMpi::isend(MPI_COMM_WORLD, job.getJobTree().getRightChildNodeRank(), msgTag, IntVec({jobId}));
        log(LOG_ADD_DESTRANK | V4_VVER, "Propagate interruption of %s ...", job.getJobTree().getRightChildNodeRank(), job.toStr());
    }
    for (auto childRank : job.getJobTree().getPastChildren()) {
        MyMpi::isend(MPI_COMM_WORLD, childRank, msgTag, IntVec({jobId}));
        log(LOG_ADD_DESTRANK | V4_VVER, "Propagate interruption of %s (past child) ...", childRank, job.toStr());
    }
    job.getJobTree().getPastChildren().clear();

    // Stop, and possibly terminate, the job
    _job_db.stop(jobId, terminate);
}

void Worker::informClientJobIsDone(int jobId, int clientRank) {
    const JobResult& result = _job_db.get(jobId).getResult();

    // Send "Job done!" with advertised result size to client
    log(LOG_ADD_DESTRANK | V4_VVER, "%s : send JOB_DONE to client", clientRank, _job_db.get(jobId).toStr());
    IntPair payload(jobId, result.getTransferSize());
    MyMpi::isend(MPI_COMM_WORLD, clientRank, MSG_NOTIFY_JOB_DONE, payload);
}

void Worker::timeoutJob(int jobId) {
    // "Virtual self message" aborting the job
    IntVec payload({jobId});
    MessageHandle handle(MyMpi::nextHandleId());
    handle.tag = MSG_NOTIFY_JOB_ABORTING;
    handle.finished = true;
    handle.receiveSelfMessage(payload.serialize(), _world_rank);
    handleNotifyJobAborting(handle);
    if (_params.isNotNull("mono")) {
        // Single job hit a limit, so there is no solution to be reported:
        // begin to propagate exit signal
        MyMpi::isend(MPI_COMM_WORLD, 0, MSG_DO_EXIT, IntVec({0}));
    }
}

void Worker::applyBalancing() {
    
    // Update volumes found during balancing, and trigger job expansions / shrinkings
    for (const auto& [jobId, volume] : _job_db.getBalancingResult()) {
        /*
        if (_job_db.has(jobId) && _job_db.get(jobId).getVolume() != volume) {
            log(
                _job_db.get(jobId).getJobTree().isRoot() ? V2_INFO : V4_VVER, 
                "#%i : update v=%i", jobId, volume
            );
        }
        */
        updateVolume(jobId, volume);
    }
}

bool Worker::checkTerminate(float time) {
    bool terminate = false;
    if (Terminator::isTerminating()) terminate = true;
    if (_global_timeout > 0 && time > _global_timeout) {
        terminate = true;
    }
    if (terminate) {
        log(_world_rank == 0 ? V2_INFO : V3_VERB, "Terminating.\n");
        Terminator::setTerminating();
        return true;
    }
    return false;
}

void Worker::createExpanderGraph() {

    // Pick fixed number k of bounce destinations
    int numBounceAlternatives = _params.getIntParam("ba");
    int numWorkers = MyMpi::size(_comm);
    if (numWorkers == 1) return; // no hops

    // Check validity of num bounce alternatives
    if (2*numBounceAlternatives > numWorkers) {
        numBounceAlternatives = numWorkers / 2;
        log(V1_WARN, "[WARN] Num bounce alternatives must be at most half the number of workers!\n");
        log(V1_WARN, "[WARN] Falling back to safe value r=%i.\n", numBounceAlternatives);
    }  

    // Create graph, get outgoing edges from this node
    _hop_destinations = AdjustablePermutation::createExpanderGraph(numWorkers, numBounceAlternatives, _world_rank);
    assert((int)_hop_destinations.size() == numBounceAlternatives);

    // Output found bounce alternatives
    std::string info = "";
    for (size_t i = 0; i < _hop_destinations.size(); i++) {
        info += std::to_string(_hop_destinations[i]) + " ";
    }
    log(V3_VERB, "My bounce alternatives: %s\n", info.c_str());
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

    Terminator::setTerminating();

    if (_mpi_monitor_thread.joinable()) _mpi_monitor_thread.join();

    log(V4_VVER, "Destruct worker\n");

    if (_params.isNotNull("mono") && _params.getParam("appmode") != "fork") {
        // Terminate directly without destructing resident job
        MPI_Finalize();
        Process::doExit(0);
    }
}