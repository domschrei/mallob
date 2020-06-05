
#include <cmath>
#include <thread>
#include <unistd.h>
#include <iostream>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <sys/syscall.h>

#include "worker.hpp"
#include "app/sat/threaded_sat_job.hpp"
#include "app/sat/forked_sat_job.hpp"
#include "data/serializable.hpp"
#include "util/sys/timer.hpp"
#include "util/console.hpp"
#include "util/random.hpp"
#include "util/sys/proc.hpp"
#include "util/sat_reader.hpp"
#include "util/sys/fork.hpp"
#include "comm/mpi_monitor.hpp"
#include "balancing/cutoff_priority_balancer.hpp"
#include "balancing/event_driven_balancer.hpp"
#include "data/job_description.hpp"

void Worker::init() {
    
    // Initialize pseudo-random order of nodes
    if (_params.isSet("derandomize")) {
        createExpanderGraph();
    }

    // Begin listening to an incoming message
    _msg_handler.registerCallback(MSG_ACCEPT_ADOPTION_OFFER, 
        [&](auto h) {handleAcceptAdoptionOffer(h);});
    _msg_handler.registerCallback(MSG_CONFIRM_ADOPTION, 
        [&](auto h) {handleConfirmAdoption(h);});
    _msg_handler.registerCallback(MSG_CONFIRM_JOB_REVISION_DETAILS, 
        [&](auto h) {handleConfirmJobRevisionDetails(h);});
    _msg_handler.registerCallback(MSG_DO_EXIT, 
        [&](auto h) {handleDoExit(h);});
    _msg_handler.registerCallback(MSG_NOTIFY_JOB_ABORTING, 
        [&](auto h) {handleNotifyJobAborting(h);});
    _msg_handler.registerCallback(MSG_NOTIFY_JOB_DONE, 
        [&](auto h) {handleNotifyJobDone(h);});
    _msg_handler.registerCallback(MSG_NOTIFY_JOB_REVISION, 
        [&](auto h) {handleNotifyJobRevision(h);});
    _msg_handler.registerCallback(MSG_NOTIFY_JOB_TERMINATING, 
        [&](auto h) {handleNotifyJobTerminating(h);});
    _msg_handler.registerCallback(MSG_NOTIFY_NODE_LEAVING_JOB, 
        [&](auto h) {handleNotifyNodeLeavingJob(h);});
    _msg_handler.registerCallback(MSG_NOTIFY_RESULT_FOUND, 
        [&](auto h) {handleNotifyResultFound(h);});
    _msg_handler.registerCallback(MSG_NOTIFY_RESULT_OBSOLETE, 
        [&](auto h) {handleNotifyResultObsolete(h);});
    _msg_handler.registerCallback(MSG_NOTIFY_VOLUME_UPDATE, 
        [&](auto h) {handleNotifyVolumeUpdate(h);});
    _msg_handler.registerCallback(MSG_OFFER_ADOPTION, 
        [&](auto h) {handleOfferAdoption(h);});
    _msg_handler.registerCallback(MSG_QUERY_JOB_RESULT, 
        [&](auto h) {handleQueryJobResult(h);});
    _msg_handler.registerCallback(MSG_QUERY_JOB_REVISION_DETAILS, 
        [&](auto h) {handleQueryJobRevisionDetails(h);});
    _msg_handler.registerCallback(MSG_QUERY_VOLUME, 
        [&](auto h) {handleQueryVolume(h);});
    _msg_handler.registerCallback(MSG_REJECT_ADOPTION_OFFER, 
        [&](auto h) {handleRejectAdoptionOffer(h);});
    _msg_handler.registerCallback(MSG_REJECT_ONESHOT, 
        [&](auto h) {handleRejectOneshot(h);});
    _msg_handler.registerCallback(MSG_REQUEST_NODE, 
        [&](auto h) {handleRequestNode(h, /*oneshot=*/false);});
    _msg_handler.registerCallback(MSG_REQUEST_NODE_ONESHOT, 
        [&](auto h) {handleRequestNode(h, /*oneshot=*/true);});
    _msg_handler.registerCallback(MSG_SEND_APPLICATION_MESSAGE, 
        [&](auto h) {handleSendApplicationMessage(h);});
    _msg_handler.registerCallback(MSG_SEND_CLIENT_RANK, 
        [&](auto h) {handleSendClientRank(h);});
    _msg_handler.registerCallback(MSG_SEND_JOB_DESCRIPTION, 
        [&](auto h) {handleSendJob(h);});
    _msg_handler.registerCallback(MSG_SEND_JOB_REVISION_DETAILS, 
        [&](auto h) {handleSendJobRevisionDetails(h);});
    _msg_handler.registerCallback(MSG_SEND_JOB_REVISION_DATA, 
        [&](auto h) {handleSendJobRevisionData(h);});
    _msg_handler.registerCallback(MSG_SEND_JOB_RESULT, 
        [&](auto h) {handleSendJobResult(h);});
    auto balanceCb = [&](MessageHandlePtr& handle) {
        if (_job_db.continueBalancing(handle)) applyBalancing();
    };
    _msg_handler.registerCallback(MSG_COLLECTIVE_OPERATION, balanceCb);
    _msg_handler.registerCallback(MSG_REDUCE_DATA, balanceCb);
    _msg_handler.registerCallback(MSG_BROADCAST_DATA, balanceCb);
    _msg_handler.registerCallback(MSG_WARMUP, [&](auto h) {
        Console::log_recv(Console::VVVERB, h->source, "Warmup msg");
    });
    _msg_handler.registerCallback(MessageHandler::TAG_DEFAULT, [&](auto h) {
        Console::log_recv(Console::WARN, h->source, "[WARN] Unknown message tag %i", h->tag);
    });
    MyMpi::beginListening();

    // Send warm-up messages with your pseudorandom bounce destinations
    if (_params.isSet("derandomize") && _params.isSet("warmup")) {
        IntVec payload({1, 2, 3, 4, 5, 6, 7, 8});
        int numRuns = 5;
        for (int run = 0; run < numRuns; run++) {
            for (auto rank : _hop_destinations) {
                MyMpi::isend(MPI_COMM_WORLD, rank, MSG_WARMUP, payload);
                Console::log_send(Console::VVERB, rank, "Warmup msg");
            }
        }
    }

    Console::log(Console::VERB, "Global init barrier ...");
    MPI_Barrier(MPI_COMM_WORLD);
    Console::log(Console::VERB, "Passed global init barrier");

    if (_params.isSet("mmpi")) _mpi_monitor_thread = std::thread(mpiMonitor, this);
    else MyMpi::_monitor_off = true;

    // Initiate single instance solving as the "root node"
    if (_params.isSet("sinst") && _world_rank == 0) {

        std::string instanceFilename = _params.getParam("sinst");
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
    bool doYield = _params.isSet("yield");

    while (!checkTerminate()) {

        if (sleepMicrosecs > 0) usleep(sleepMicrosecs);
        if (doYield) std::this_thread::yield();
        
        float time = Timer::elapsedSeconds();

        // Poll received messages, make progress in sent messages
        _msg_handler.pollMessages(time);
        MyMpi::testSentHandles();

        // Increment iteration modulo ten;
        // only continue job checking etc. at every tenth iteration
        iteration = (iteration == 9 ? 0 : iteration+1);
        if (iteration > 0) continue; 

        if (time - lastMemCheckTime > memCheckPeriod) {
            lastMemCheckTime = time;
            // Print stats

            // For this process
            double vm_usage, resident_set; int cpu;
            Proc::getSelfMemAndSchedCpu(cpu, vm_usage, resident_set);
            vm_usage *= 0.001 * 0.001;
            resident_set *= 0.001 * 0.001;
            Console::log(Console::VERB, "mem cpu=%i vm=%.4fGB rss=%.4fGB", cpu, vm_usage, resident_set);
            _sys_state.setLocal(2, resident_set);

            // For this "management" thread
            double perc_cpu; float sysShare;
            bool success = Proc::getThreadCpuRatio(Proc::getTid(), perc_cpu, sysShare);
            if (success) {
                Console::log(Console::VERB, "main : %.2f%% CPU -> %.2f%% systime", perc_cpu, 100*sysShare);
            }

            // For the current job
            if (!_job_db.isIdle()) _job_db.getActive().appl_dumpStats();

            // Forget jobs that are old or wasting memory
            _job_db.forgetOldJobs();
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
                _sys_state.setLocal(0, 0.0f); // busy nodes
                _sys_state.setLocal(1, 0.0f); // active jobs

            } else {
                Job &job = _job_db.getActive();
                int id = job.getId();

                _sys_state.setLocal(0, 1.0f); // busy nodes
                _sys_state.setLocal(1, job.isRoot() ? 1.0f : 0.0f); // active jobs

                bool abort = false;
                if (job.isRoot()) abort = _job_db.checkComputationLimits(id);
                if (abort) {
                    // Timeout (CPUh or wallclock time) hit
                    timeoutJob(id);
                } else {
                    // Finish initialization as needed
                    if (job.isDoneInitializing()) {
                        job.endInitialization();
                        if (job.isRoot()) {
                            // Root worker finished initialization: begin growing if applicable
                            if (_job_db.hasVolume(id)) updateVolume(id, _job_db.getVolume(id));
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
            Console::log(verb, "sysstate busy=%.2f%% jobs=%i accmem=%.2fGB", 
                        100*result[0]/MyMpi::size(_comm), (int)result[1], result[2]);
        }
    }

    Console::flush();
    fflush(stdout);
}

void Worker::handleNotifyJobAborting(MessageHandlePtr& handle) {

    int jobId = Serializable::get<int>(*handle->recvData);
    if (!_job_db.has(jobId)) return;

    interruptJob(jobId, /*terminate=*/true, /*reckless=*/true);
    
    if (_job_db.get(jobId).isRoot()) {
        // Forward information on aborted job to client
        MyMpi::isend(MPI_COMM_WORLD, _job_db.get(jobId).getParentNodeRank(), MSG_NOTIFY_JOB_ABORTING, handle->recvData);
    }
}

void Worker::handleAcceptAdoptionOffer(MessageHandlePtr& handle) {

    // Retrieve according job commitment
    JobSignature sig = Serializable::get<JobSignature>(*handle->recvData);
    if (!_job_db.hasCommitment(sig.jobId)) {
        Console::log(Console::WARN, "[WARN] Job commitment for #%i not present despite adoption accept msg", sig.jobId);
        return;
    }

    JobRequest& req = _job_db.getCommitment(sig.jobId);

    if (req.fullTransfer == 1) {
        // Full transfer of job description is required:
        // Send ACK to parent and receive full job description
        Console::log(Console::VERB, "Will receive desc. of #%i, size %i", req.jobId, sig.getTransferSize());
        MyMpi::irecv(MPI_COMM_WORLD, handle->source, MSG_SEND_JOB_DESCRIPTION, sig.getTransferSize()); // to be received later
        MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_CONFIRM_ADOPTION, req);
    } else {
        _job_db.reactivate(req, handle->source);
        if (!_job_db.isIdle() && _job_db.getActive().getId() == req.jobId) {
            // Query volume of job
            MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_QUERY_VOLUME, IntVec({req.jobId}));
        }
    }
}

void Worker::handleConfirmJobRevisionDetails(MessageHandlePtr& handle) {
    IntVec response(*handle->recvData);
    int jobId = response[0];
    int firstRevision = response[1];
    int lastRevision = response[2];
    MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_SEND_JOB_REVISION_DATA, 
                _job_db.get(jobId).getDescription().serialize(firstRevision, lastRevision));
}

void Worker::handleConfirmAdoption(MessageHandlePtr& handle) {
    JobRequest req = Serializable::get<JobRequest>(*handle->recvData);

    // If job already terminated, the description contains the job id ONLY
    if (!_job_db.has(req.jobId)) {
        MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_SEND_JOB_DESCRIPTION, IntVec({req.jobId}));
        return;
    }
    Job& job = _job_db.get(req.jobId);
    if (job.isPast() || job.isForgetting()) {
        MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_SEND_JOB_DESCRIPTION, IntVec({req.jobId}));
        return;
    }

    // Retrieve and send concerned job description
    MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_SEND_JOB_DESCRIPTION, job.getSerializedDescription());
    Console::log_send(Console::VERB, handle->source, "Sent job desc. of %s", job.toStr());

    // Mark new node as one of the node's children
    if (req.requestedNodeIndex == job.getLeftChildIndex()) {
        job.setLeftChild(handle->source);
    } else if (req.requestedNodeIndex == job.getRightChildIndex()) {
        job.setRightChild(handle->source);
    } else assert(req.requestedNodeIndex == 0);
}

void Worker::handleDoExit(MessageHandlePtr& handle) {
    Console::log_recv(Console::VERB, handle->source, "Received exit signal");

    // Forward exit signal
    if (_world_rank*2+1 < MyMpi::size(MPI_COMM_WORLD))
        MyMpi::isend(MPI_COMM_WORLD, _world_rank*2+1, MSG_DO_EXIT, handle->recvData);
    if (_world_rank*2+2 < MyMpi::size(MPI_COMM_WORLD))
        MyMpi::isend(MPI_COMM_WORLD, _world_rank*2+2, MSG_DO_EXIT, handle->recvData);
    while (MyMpi::hasOpenSentHandles()) MyMpi::testSentHandles();

    _exiting = true;
}

void Worker::handleRejectOneshot(MessageHandlePtr& handle) {
    JobRequest req = Serializable::get<JobRequest>(*handle->recvData);
    Console::log_recv(Console::VVVERB, handle->source, 
        "%s rejected by dormant child", _job_db.toStr(req.jobId, req.requestedNodeIndex).c_str());

    if (_job_db.isAdoptionOfferObsolete(req)) return;

    Job& job = _job_db.get(req.jobId);
    job.addFailToDormantChild(handle->source);

    bool doNormalHopping = false;
    if (req.numHops > std::max(_params.getIntParam("jc"), 2)) {
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
            MyMpi::isend(MPI_COMM_WORLD, rank, MSG_REQUEST_NODE_ONESHOT, req);
            Console::log_send(Console::VERB, rank, "%s : query dormant child", job.toStr());
        }
    }

    if (doNormalHopping) {
        Console::log(Console::VERB, "%s : switch to normal hops", job.toStr());
        req.numHops = -1;
        bounceJobRequest(req, handle->source);
    }
}

void Worker::handleRequestNode(MessageHandlePtr& handle, bool oneshot) {

    JobRequest req = Serializable::get<JobRequest>(*handle->recvData);

    // Discard request if it has become obsolete
    if (_job_db.isRequestObsolete(req)) {
        Console::log_recv(Console::VERB, handle->source, "Discard req. %s from time %.2f", 
                    _job_db.toStr(req.jobId, req.requestedNodeIndex).c_str(), req.timeOfBirth);
        return;
    }

    int removedJob;
    bool adopts = _job_db.tryAdopt(req, oneshot, removedJob);
    if (adopts) {
        if (removedJob >= 0) {
            Job& job = _job_db.get(removedJob);
            IntPair pair(job.getId(), job.getIndex());
            MyMpi::isend(MPI_COMM_WORLD, job.getParentNodeRank(), MSG_NOTIFY_NODE_LEAVING_JOB, pair);
        }

        // Adoption takes place
        std::string jobstr = _job_db.toStr(req.jobId, req.requestedNodeIndex);
        Console::log_recv(Console::VERB, handle->source, "Adopting %s after %i hops", jobstr.c_str(), req.numHops);
        assert(_job_db.isIdle() || Console::fail("Adopting a job, but not idle!"));

        // Commit on the job, send a request to the parent
        bool fullTransfer = false;
        if (!_job_db.has(req.jobId)) {
            // Job is not known yet: create instance, request full transfer
            _job_db.createJob(MyMpi::size(_comm), _world_rank, req.jobId);
            fullTransfer = true;
        } else if (!_job_db.get(req.jobId).hasJobDescription() && !_job_db.get(req.jobId).isInitializing()) {
            // Job is known, but never received full description:
            // request full transfer
            fullTransfer = true;
        }
        req.fullTransfer = fullTransfer ? 1 : 0;
        _job_db.commit(req);
        MyMpi::isend(MPI_COMM_WORLD, req.requestingNodeRank, MSG_OFFER_ADOPTION, req);

    } else {
        if (oneshot) {
            Console::log_send(Console::VVVERB, handle->source, "decline oneshot request for %s", 
                        _job_db.toStr(req.jobId, req.requestedNodeIndex).c_str());
            MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_REJECT_ONESHOT, handle->recvData);
        } else {
            // Continue job finding procedure somewhere else
            bounceJobRequest(req, handle->source);
        }
    }
}

void Worker::handleSendClientRank(MessageHandlePtr& handle) {

    // Receive rank of the job's client
    IntPair recv(*handle->recvData);
    int jobId = recv.first;
    int clientRank = recv.second;
    assert(_job_db.has(jobId));

    // Inform client of the found job result
    informClientJobIsDone(jobId, clientRank);
}

void Worker::handleIncrementalJobFinished(MessageHandlePtr& handle) {
    interruptJob(Serializable::get<int>(*handle->recvData), /*terminate=*/true, /*reckless=*/false);
}

void Worker::handleInterrupt(MessageHandlePtr& handle) {
    interruptJob(Serializable::get<int>(*handle->recvData), /*terminate=*/false, /*reckless=*/false);
}

void Worker::handleSendApplicationMessage(MessageHandlePtr& handle) {

    // Deserialize job-specific message
    JobMessage msg = Serializable::get<JobMessage>(*handle->recvData);
    int jobId = msg.jobId;
    if (!_job_db.has(jobId)) {
        Console::log(Console::WARN, "[WARN] Job message from unknown job #%i", jobId);
        return;
    }
    // Give message to corresponding job
    Job& job = _job_db.get(jobId);
    if (job.isActive()) job.appl_communicate(handle->source, msg);
}

void Worker::handleNotifyJobDone(MessageHandlePtr& handle) {
    IntPair recv(*handle->recvData);
    int jobId = recv.first;
    int resultSize = recv.second;
    Console::log_recv(Console::VERB, handle->source, "Will receive job result, length %i, for job #%i", resultSize, jobId);
    MyMpi::irecv(MPI_COMM_WORLD, handle->source, MSG_SEND_JOB_RESULT, resultSize);
    MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_QUERY_JOB_RESULT, handle->recvData);
}

void Worker::handleNotifyJobRevision(MessageHandlePtr& handle) {
    IntVec payload(*handle->recvData);
    int jobId = payload[0];
    int revision = payload[1];

    assert(_job_db.has(jobId));
    assert(_job_db.get(jobId).isInState({STANDBY}) || Console::fail("%s : state %s", 
            _job_db.get(jobId).toStr(), jobStateStrings[_job_db.get(jobId).getState()]));

    // Request receiving information on revision size
    Job& job = _job_db.get(jobId);
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

void Worker::handleOfferAdoption(MessageHandlePtr& handle) {

    JobRequest req = Serializable::get<JobRequest>(*handle->recvData);
    Console::log_recv(Console::VERB, handle->source, "Adoption offer for %s", 
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

            // If req.fullTransfer, then wait for the child to acknowledge having received the signature
            if (req.fullTransfer == 1) {
                Console::log_send(Console::VERB, handle->source, "Will send desc. of %s", 
                        _job_db.toStr(req.jobId, req.requestedNodeIndex).c_str());
            } else {
                Console::log_send(Console::VERB, handle->source, "Resume child %s", 
                        _job_db.toStr(req.jobId, req.requestedNodeIndex).c_str());
            }
            // Child *will* start / resume its job solvers.
            // Mark new node as one of the node's children
            if (req.requestedNodeIndex == job.getLeftChildIndex()) {
                job.setLeftChild(handle->source);
            } else if (req.requestedNodeIndex == job.getRightChildIndex()) {
                job.setRightChild(handle->source);
            } else assert(req.requestedNodeIndex == 0);
        }
    }

    // If rejected: Send message to rejected child node
    if (reject) {
        MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_REJECT_ADOPTION_OFFER, req);
    }
}

void Worker::handleQueryJobResult(MessageHandlePtr& handle) {

    // Receive acknowledgement that the client received the advertised result size
    // and wishes to receive the full job result
    int jobId = Serializable::get<int>(*handle->recvData);
    assert(_job_db.has(jobId));
    const JobResult& result = _job_db.get(jobId).getResult();
    Console::log_send(Console::VERB, handle->source, "Send full result to client");
    MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_SEND_JOB_RESULT, result);
    _job_db.get(jobId).setResultTransferPending(false);
}

void Worker::handleQueryJobRevisionDetails(MessageHandlePtr& handle) {
    IntVec request(*handle->recvData);
    int jobId = request[0];
    int firstRevision = request[1];
    int lastRevision = request[2];

    assert(_job_db.has(jobId));

    JobDescription& desc = _job_db.get(jobId).getDescription();
    IntVec response({jobId, firstRevision, lastRevision, desc.getTransferSize(firstRevision, lastRevision)});
    MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_SEND_JOB_REVISION_DETAILS, response);
}

void Worker::handleQueryVolume(MessageHandlePtr& handle) {

    IntVec payload(*handle->recvData);
    int jobId = payload[0];

    // No volume of this job (yet?) -- ignore.
    if (!_job_db.has(jobId) || !_job_db.hasVolume(jobId)) return;

    int volume = _job_db.getVolume(jobId);
    IntVec response({jobId, volume});

    Console::log_send(Console::VERB, handle->source, "Answer #%i volume query with v=%i", jobId, volume);
    MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_NOTIFY_VOLUME_UPDATE, response);
}

void Worker::handleRejectAdoptionOffer(MessageHandlePtr& handle) {

    // Retrieve committed job
    JobRequest req = Serializable::get<JobRequest>(*handle->recvData);
    if (!_job_db.has(req.jobId)) return;
    Job &job = _job_db.get(req.jobId);
    if (!job.isCommitted()) return; // Commitment was already erased

    // Erase commitment
    Console::log_recv(Console::VERB, handle->source, "Rejected to become %s : uncommitting", job.toStr());
    _job_db.uncommit(req.jobId);
}

void Worker::handleNotifyResultObsolete(MessageHandlePtr& handle) {

    IntVec res(*handle->recvData);
    int jobId = res[0];
    //int revision = res[1];
    if (!_job_db.has(jobId)) return;
    Console::log_recv(Console::VERB, handle->source, "job result for %s unwanted", _job_db.get(jobId).toStr());
    _job_db.get(jobId).setResultTransferPending(false);
}

void Worker::handleSendJob(MessageHandlePtr& handle) {
    auto data = handle->recvData;
    Console::log_recv(Console::VVVERB, handle->source, "Receiving some desc. of size %i", data->size());
    int jobId = Serializable::get<int>(*data);
    _job_db.init(jobId, data, handle->source);
}

void Worker::handleSendJobResult(MessageHandlePtr& handle) {
    JobResult jobResult = Serializable::get<JobResult>(*handle->recvData);
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

    if (_params.isSet("sinst")) {
        // Single instance solving is done: begin exit signal
        MyMpi::isend(MPI_COMM_WORLD, 0, MSG_DO_EXIT, IntVec({0}));
    }
}

void Worker::handleSendJobRevisionData(MessageHandlePtr& handle) {
    int jobId = Serializable::get<int>(*handle->recvData);
    assert(_job_db.has(jobId) && _job_db.get(jobId).isInState({STANDBY}));

    // TODO in separate thread
    Job& job = _job_db.get(jobId);
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

void Worker::handleSendJobRevisionDetails(MessageHandlePtr& handle) {
    IntVec response(*handle->recvData);
    int transferSize = response[3];
    MyMpi::irecv(MPI_COMM_WORLD, handle->source, MSG_SEND_JOB_REVISION_DATA, transferSize);
    MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_CONFIRM_JOB_REVISION_DETAILS, handle->recvData);
}

void Worker::handleNotifyJobTerminating(MessageHandlePtr& handle) {
    interruptJob(Serializable::get<int>(*handle->recvData), /*terminate=*/true, /*reckless=*/false);
}

void Worker::handleNotifyVolumeUpdate(MessageHandlePtr& handle) {
    IntPair recv(*handle->recvData);
    int jobId = recv.first;
    int volume = recv.second;
    if (!_job_db.has(jobId)) {
        Console::log(Console::WARN, "[WARN] Volume update for unknown #%i", jobId);
        return;
    }

    // Update volume assignment inside balancer and in actual job instance
    // (and its children)
    _job_db.overrideBalancerVolume(jobId, volume);
    updateVolume(jobId, volume);
}

void Worker::handleNotifyNodeLeavingJob(MessageHandlePtr& handle) {

    // Retrieve job
    IntPair recv(*handle->recvData);
    int jobId = recv.first;
    int index = recv.second;
    if (!_job_db.has(jobId)) return;
    Job& job = _job_db.get(jobId);

    // Which child is defecting?
    if (job.hasLeftChild() && job.getLeftChildIndex() == index && job.getLeftChildNodeRank() == handle->source) {
        // Prune left child
        job.unsetLeftChild();
    } else if (job.hasRightChild() && job.getRightChildIndex() == index && job.getRightChildNodeRank() == handle->source) {
        // Prune right child
        job.unsetRightChild();
    } else {
        Console::log(Console::VERB, "%s : unknown child %s defecting", job.toStr(), _job_db.toStr(jobId, index).c_str());
    }

    // If necessary, find replacement
    if (index < job.getLastVolume()) {

        int nextNodeRank = -1;
        int tag = MSG_REQUEST_NODE;

        // Try to find a dormant child that is not the message source
        std::set<int> dormantChildren = job.getDormantChildren();
        for (int child : dormantChildren) {
            if (child != handle->source) {
                nextNodeRank = child;
                tag = MSG_REQUEST_NODE_ONESHOT;
                break;
            }
        }
        // Otherwise, pick a random node
        if (nextNodeRank == -1 && _params.isSet("derandomize")) {
            nextNodeRank = Random::choice(_hop_destinations);
        } else if (nextNodeRank == -1) {
            nextNodeRank = getRandomNonSelfWorkerNode();
        }

        // Initiate search for a replacement for the defected child
        JobRequest req(jobId, job.getRootNodeRank(), _world_rank, index, Timer::elapsedSeconds(), 0);
        Console::log_send(Console::VERB, nextNodeRank, "%s : try to find child replacing defected %s", 
                        job.toStr(), _job_db.toStr(jobId, index).c_str());
        MyMpi::isend(MPI_COMM_WORLD, nextNodeRank, tag, req);
    }
}

void Worker::handleNotifyResultFound(MessageHandlePtr& handle) {

    // Retrieve job
    IntVec res(*handle->recvData);
    int jobId = res[0];
    int revision = res[1];
    if (!_job_db.has(jobId) || !_job_db.get(jobId).isRoot()) {
        Console::log(Console::WARN, "[WARN] Invalid adressee for job result of #%i", jobId);
        MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_NOTIFY_RESULT_OBSOLETE, handle->recvData);
        return;
    }
    if (_job_db.get(jobId).isPast()) {
        Console::log_recv(Console::VERB, handle->source, "Discard obsolete result for job #%i", jobId);
        MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_NOTIFY_RESULT_OBSOLETE, handle->recvData);
        return;
    }
    if (_job_db.get(jobId).getRevision() > revision) {
        Console::log_recv(Console::VERB, handle->source, "Discard obsolete result for job #%i rev. %i", jobId, revision);
        MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_NOTIFY_RESULT_OBSOLETE, handle->recvData);
        return;
    }
    
    Console::log_recv(Console::VERB, handle->source, "Result found for job #%i", jobId);

    // Terminate job and propagate termination message
    if (_job_db.get(jobId).getDescription().isIncremental()) {
        handleInterrupt(handle);
    } else {
        handleNotifyJobTerminating(handle);
    }

    // Try to send away termination message as fast as possible
    // to make space for other expanding jobs
    MyMpi::testSentHandles();

    // Redirect termination signal
    IntPair payload(jobId, _job_db.get(jobId).getParentNodeRank());
    if (handle->source == _world_rank) {
        // Self-message of root node: Directly send termination message to client
        informClientJobIsDone(payload.first, payload.second);
    } else {
        // Send rank of client node to the finished worker,
        // such that the worker can inform the client of the result
        MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_SEND_CLIENT_RANK, payload);
        Console::log_send(Console::VERB, handle->source, "Forward rank of client (%i)", payload.second); 
    }
}

void Worker::bounceJobRequest(JobRequest& request, int senderRank) {

    // Increment #hops
    request.numHops++;
    int num = request.numHops;

    // Show warning if #hops is a large power of two
    if ((num >= 512) && ((num & (num - 1)) == 0)) {
        Console::log(Console::WARN, "[WARN] Hop no. %i for %s", num, _job_db.toStr(request.jobId, request.requestedNodeIndex).c_str());
    }

    int nextRank;
    if (_params.isSet("derandomize")) {
        // Get random choice from bounce alternatives
        nextRank = Random::choice(_hop_destinations);
        // while skipping the requesting node and the sender
        while (nextRank == request.requestingNodeRank || nextRank == senderRank) {
            nextRank = Random::choice(_hop_destinations);
        }
    } else {
        // Generate pseudorandom permutation of this request
        int n = MyMpi::size(_comm);
        AdjustablePermutation perm(n, 3 * request.jobId + 7 * request.requestedNodeIndex + 11 * request.requestingNodeRank);
        // Fetch next index of permutation based on number of hops
        int permIdx = request.numHops % n;
        nextRank = perm.get(permIdx);
        // (while skipping yourself, the requesting node, and the sender)
        while (nextRank == _world_rank || nextRank == request.requestingNodeRank || nextRank == senderRank) {
            permIdx = (permIdx+1) % n;
            nextRank = perm.get(permIdx);
        }
    }

    // Send request to "next" worker node
    Console::log_send(Console::VVVERB, nextRank, "Hop %s", _job_db.toStr(request.jobId, request.requestedNodeIndex).c_str());
    MyMpi::isend(MPI_COMM_WORLD, nextRank, MSG_REQUEST_NODE, request);
}

void Worker::updateVolume(int jobId, int volume) {

    if (!_job_db.has(jobId)) return;
    Job &job = _job_db.get(jobId);

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
            MyMpi::isend(MPI_COMM_WORLD, ranks[i], MSG_NOTIFY_VOLUME_UPDATE, payload);

        } else if (job.hasJobDescription() && nextIndex < volume) {
            // Grow
            Console::log(Console::VVVERB, "%s : grow", job.toStr());
            JobRequest req(jobId, job.getRootNodeRank(), _world_rank, nextIndex, Timer::elapsedSeconds(), 0);
            int nextNodeRank, tag;
            if (dormantChildren.empty()) {
                tag = MSG_REQUEST_NODE;
                ranks[i] = i == 0 ? job.getLeftChildNodeRank() : job.getRightChildNodeRank();
                nextNodeRank = ranks[i];
            } else {
                tag = MSG_REQUEST_NODE_ONESHOT;
                nextNodeRank = Random::choice(dormantChildren);
                dormantChildren.erase(nextNodeRank);
            }
            Console::log_send(Console::VERB, nextNodeRank, "%s : initiate growth by %s", 
                        job.toStr(), _job_db.toStr(job.getId(), nextIndex).c_str());
            MyMpi::isend(MPI_COMM_WORLD, nextNodeRank, tag, req);
        }
    }

    // Shrink (and pause solving) if necessary
    if (thisIndex > 0 && thisIndex >= volume) {
        _job_db.suspend(jobId);
        MyMpi::isend(MPI_COMM_WORLD, job.getParentNodeRank(), MSG_NOTIFY_NODE_LEAVING_JOB, IntPair(jobId, thisIndex));
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
    if (job.hasLeftChild()) {
        MyMpi::isend(MPI_COMM_WORLD, job.getLeftChildNodeRank(), msgTag, IntVec({jobId}));
        Console::log_send(Console::VERB, job.getLeftChildNodeRank(), "Propagate interruption of %s ...", job.toStr());
    }
    if (job.hasRightChild()) {
        MyMpi::isend(MPI_COMM_WORLD, job.getRightChildNodeRank(), msgTag, IntVec({jobId}));
        Console::log_send(Console::VERB, job.getRightChildNodeRank(), "Propagate interruption of %s ...", job.toStr());
    }
    for (auto childRank : job.getPastChildren()) {
        MyMpi::isend(MPI_COMM_WORLD, childRank, msgTag, IntVec({jobId}));
        Console::log_send(Console::VERB, childRank, "Propagate interruption of %s (past child) ...", job.toStr());
    }
    job.getPastChildren().clear();

    // Try to send away termination message as fast as possible
    // to make space for other expanding jobs
    MyMpi::testSentHandles();

    // Stop, and possibly terminate, the job
    _job_db.stop(jobId, terminate);
}

void Worker::informClientJobIsDone(int jobId, int clientRank) {
    const JobResult& result = _job_db.get(jobId).getResult();

    // Send "Job done!" with advertised result size to client
    Console::log_send(Console::VERB, clientRank, "%s : send JOB_DONE to client", _job_db.get(jobId).toStr());
    IntPair payload(jobId, result.getTransferSize());
    MyMpi::isend(MPI_COMM_WORLD, clientRank, MSG_NOTIFY_JOB_DONE, payload);
}

void Worker::timeoutJob(int jobId) {
    // "Virtual self message" aborting the job
    IntVec payload({jobId, _job_db.get(jobId).getRevision()});
    MessageHandlePtr handle(new MessageHandle(MyMpi::nextHandleId()));
    handle->source = _world_rank;
    handle->recvData = payload.serialize();
    handle->tag = MSG_NOTIFY_JOB_ABORTING;
    handle->finished = true;
    handleNotifyJobAborting(handle);
}

void Worker::applyBalancing() {
    
    // Update volumes found during balancing, and trigger job expansions / shrinkings
    for (const auto& pair : _job_db.getBalancingResult()) {
        int jobId = pair.first;
        int volume = pair.second;
        if (_job_db.has(jobId) && _job_db.get(jobId).getLastVolume() != volume) {
            Console::log(Console::INFO, "#%i : update v=%i", jobId, volume);
        }
        updateVolume(jobId, volume);
    }
}

bool Worker::checkTerminate() {
    if (_exiting) return true;
    if (_global_timeout > 0 && Timer::elapsedSeconds() > _global_timeout) {
        Console::log(Console::INFO, "Global timeout: terminating.");
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
    assert(_hop_destinations.size() == numBounceAlternatives);

    // Output found bounce alternatives
    std::string info = "";
    for (int i = 0; i < _hop_destinations.size(); i++) {
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
    Fork::terminateAll();
    
    // -- quicker than normal terminate
    // -- workaround for idle times after finishing
    //raise(SIGTERM);

    if (_mpi_monitor_thread.joinable())
        _mpi_monitor_thread.join();

    Console::log(Console::VVERB, "Destruct worker");
}