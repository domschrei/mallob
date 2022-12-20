
#include "anytime_sat_clause_communicator.hpp"

#include "util/logger.hpp"
#include "comm/mympi.hpp"
#include "../sharing/filter/clause_filter.hpp"
#include "util/sys/thread_pool.hpp"
#include "clause_sharing_session.hpp"

void advanceCollective(BaseSatJob* job, JobMessage& msg, int broadcastTag) {
    if (job->getJobTree().isRoot() && msg.tag != broadcastTag) {
        // Self message: Switch from reduce to broadcast
        int oldTag = msg.tag;
        msg.tag = broadcastTag;
        MyMpi::isend(job->getJobTree().getRank(), MSG_SEND_APPLICATION_MESSAGE, msg);
        msg.tag = oldTag;
    } else if (msg.tag == broadcastTag) {
        // Broadcast to children
        if (job->getJobTree().hasLeftChild())
            MyMpi::isend(job->getJobTree().getLeftChildNodeRank(), MSG_SEND_APPLICATION_MESSAGE, msg);
        if (job->getJobTree().hasRightChild())
            MyMpi::isend(job->getJobTree().getRightChildNodeRank(), MSG_SEND_APPLICATION_MESSAGE, msg);
    } else {
        // Reduce to parent
        MyMpi::isend(job->getJobTree().getParentNodeRank(), MSG_SEND_APPLICATION_MESSAGE, msg);
    }
}

void AnytimeSatClauseCommunicator::communicate() {

    if (_proof_producer) _proof_producer->advanceFileMerger();

    // inactive?
    if (!_suspended && _job->getState() != ACTIVE) {
        // suspended!
        _suspended = true;
        if (_use_cls_history) _cls_history.onSuspend();
    }
    if (_suspended) {
        if (_job->getState() == ACTIVE) _suspended = false;
    }

    // if doing certified UNSAT, advance the establishing communication
    checkCertifiedUnsatReadyMsg();

    // Advance and/or clean up current clause sharing session
    if (_current_session) {
        _current_session->advanceSharing();
        if (_current_session->isDone()) {
            _cancelled_sessions.emplace_back(_current_session.release());
        }
    }

    // clean up old sessions
    while (!_cancelled_sessions.empty()) {
        auto& session = *_cancelled_sessions.front();
        if (!session.isDestructible()) break;
        _cancelled_sessions.pop_front();
    }

    // If a previous sharing initiation message has been deferred,
    // try to activate it now (if no sessions are active any more)
    tryActivateDeferredSharingInitiation();

    // Distributed proof assembly methods
    if (_proof_producer) _proof_producer->advanceProofAssembly();

    // root: initiate sharing
    if (_job->getJobTree().isRoot() && tryInitiateSharing()) return;
}

void AnytimeSatClauseCommunicator::handle(int source, int mpiTag, JobMessage& msg) {

    assert(msg.jobId == _job->getId());
    if (handleClauseHistoryMessage(source, mpiTag, msg)) return;
    if (handleProofProductionMessage(source, mpiTag, msg)) return;
    if (handleClauseSharingMessage(source, mpiTag, msg)) return;
    assert(log_return_false("[ERROR] Unexpected job message!\n"));
}

bool AnytimeSatClauseCommunicator::handleClauseHistoryMessage(int source, int mpiTag, JobMessage& msg) {

    if (msg.tag == ClauseHistory::MSG_CLAUSE_HISTORY_SEND_CLAUSES) {
        _cls_history.addEpoch(msg.epoch, msg.payload, /*entireIndex=*/true);
        _cls_history.sendNextBatches();
        return true;
    }
    if (msg.tag == ClauseHistory::MSG_CLAUSE_HISTORY_SUBSCRIBE) {
        _cls_history.onSubscribe(source, msg.payload[0], msg.payload[1]);
        return true;
    }
    if (msg.tag == ClauseHistory::MSG_CLAUSE_HISTORY_UNSUBSCRIBE) {
        _cls_history.onUnsubscribe(source);
        return true;
    }
    return false;
}

bool AnytimeSatClauseCommunicator::handleProofProductionMessage(int source, int mpiTag, JobMessage& msg) {

    if (msg.tag == MSG_NOTIFY_READY_FOR_PROOF_SAFE_SHARING) {
        _num_ready_msgs_from_children++;
        LOG(V3_VERB, "got comm ready msg (total:%i)\n", _num_ready_msgs_from_children);
        return true;
    }

    bool forwardedInitiateProofMessage = false;
    if (msg.tag == MSG_NOTIFY_UNSAT_FOUND) {
        assert(_job->getJobTree().isRoot());
        
        if (!_sent_cert_unsat_ready_msg) {
            // Job is not ready yet to reconstruct proofs.
            LOG(V2_INFO, "Deferring UNSAT notification since job is not yet ready\n");
            _msg_unsat_found = std::move(msg);
            return true;
        }

        if (_proof_producer) {
            // Obsolete message
            LOG(V2_INFO, "Obsolete UNSAT notification - already assembling a proof\n");
            return true;
        }

        // Initiate proof assembly
        msg.tag = MSG_INITIATE_PROOF_COMBINATION;
        msg.payload.push_back(_job->getGlobalNumWorkers());
        // Use *original* #threads, not adjusted #threads,
        // since proof instance IDs are assigned that way!
        msg.payload.push_back(_params.numThreadsPerProcess());
        //msg.payload.push_back(_job->getNumThreads());
        forwardedInitiateProofMessage = true;
        // vvv Advances in the next branch vvv
    }

    if (msg.tag == MSG_INITIATE_PROOF_COMBINATION) {

        // Guarantee that the proof combination is not initiated more than once
        if (_initiated_proof_assembly) return true;
        _initiated_proof_assembly = true;
        _job->setSolvingDone();

        if (_job->getJobTree().isRoot()) {
            _solving_time = _job->getAgeSinceActivation();
            LOG(V2_INFO, "TIMING solving %.3f\n", _solving_time);
        }
        LOG(V2_INFO, "Initiate proof assembly\n");

        // Propagate initialization message
        advanceCollective(_job, msg, MSG_INITIATE_PROOF_COMBINATION);  
        if (forwardedInitiateProofMessage) {
            // send the initiation message explicitly again
            // to ensure that the job is terminated properly
            MyMpi::isend(_job->getMyMpiRank(), MSG_SEND_APPLICATION_MESSAGE, msg);
        }

        if (!_proof_producer) setupProofProducer(msg);
        return true;
    }

    if (msg.tag == MSG_ALLREDUCE_PROOF_RELEVANT_CLAUSES) {
        _proof_producer->handle(source, mpiTag, msg);
        return true;
    }

    return false;
}

bool AnytimeSatClauseCommunicator::handleClauseSharingMessage(int source, int mpiTag, JobMessage& msg) {

    // Initial signal to initiate a sharing epoch
    if (msg.tag == MSG_INITIATE_CLAUSE_SHARING) {
        initiateClauseSharing(msg);
        return true;
    }

    // Advance all-reductions
    bool success = false;
    if (_current_session) {
        success = _current_session->advanceClauseAggregation(source, mpiTag, msg)
                || _current_session->advanceFilterAggregation(source, mpiTag, msg);
    }
    return success;
}

void AnytimeSatClauseCommunicator::initiateClauseSharing(JobMessage& msg) {

    if (_current_session || !_deferred_sharing_initiation_msgs.empty()) {
        // defer message until all past sessions are done
        // and all earlier deferred initiation messages have been processed
        LOG(V3_VERB, "%s : deferring CS initiation\n", _job->toStr());
        _deferred_sharing_initiation_msgs.push_back(std::move(msg));
        return;
    }

    // no current sessions active - can start new session
    _current_epoch = msg.epoch;
    LOG(V5_DEBG, "%s : INIT COMM e=%i nc=%i\n", _job->toStr(), _current_epoch, 
        _job->getJobTree().getNumChildren());

    // extract compensation factor for this session from the message
    float compensationFactor;
    static_assert(sizeof(float) == sizeof(int));
    memcpy(&compensationFactor, msg.payload.data(), sizeof(float));
    assert(compensationFactor >= 0.1 && compensationFactor <= 10);

    _current_session.reset(
        new ClauseSharingSession(_params, _job, _cdb, _cls_history, _current_epoch, compensationFactor)
    );
    advanceCollective(_job, msg, MSG_INITIATE_CLAUSE_SHARING);
}

void AnytimeSatClauseCommunicator::tryActivateDeferredSharingInitiation() {
    
    // Anything to activate?
    if (_deferred_sharing_initiation_msgs.empty()) return;

    // cannot start new sharing if a session is still present
    if (_current_session) return;

    // sessions are empty -> WILL succeed to initiate sharing
    // -> initiation message CAN be deleted afterwards.
    JobMessage msg = std::move(_deferred_sharing_initiation_msgs.front());
    _deferred_sharing_initiation_msgs.pop_front();
    initiateClauseSharing(msg);
}

void AnytimeSatClauseCommunicator::feedHistoryIntoSolver() {
    if (_use_cls_history) _cls_history.feedHistoryIntoSolver();
}

bool AnytimeSatClauseCommunicator::tryInitiateSharing() {

    if (_proof_producer || !_sent_cert_unsat_ready_msg) return false;

    auto time = Timer::elapsedSecondsCached();
    bool nextEpochDue = time - _time_of_last_epoch_initiation >= _params.appCommPeriod();
    bool lastEpochDone = !_current_session;
    if (!nextEpochDue) return false;

    if (!lastEpochDone) {
        LOG(V1_WARN, "[WARN] %s : Next epoch over-due!\n", _job->toStr());
        return false;
    }

    _current_epoch++;

    // Assemble job message
    JobMessage msg(_job->getId(), _job->getRevision(), _current_epoch, MSG_INITIATE_CLAUSE_SHARING);
    msg.payload.push_back(0);
    float compensationFactor = _job->updateSharingCompensationFactor();
    static_assert(sizeof(float) == sizeof(int));
    memcpy(msg.payload.data(), &compensationFactor, sizeof(float));

    // Advance initiation time exactly by the specified period 
    // in order to lose no time for the subsequent epoch
    _time_of_last_epoch_initiation += _params.appCommPeriod();
    // If an epoch has already been skipped, just set the initiation to the current time
    if (time - _time_of_last_epoch_initiation >= _params.appCommPeriod())
        _time_of_last_epoch_initiation = time;

    // Self message to initiate clause sharing
    MyMpi::isend(_job->getJobTree().getRank(), MSG_SEND_APPLICATION_MESSAGE, msg);
    LOG(V4_VVER, "%s CS init\n", _job->toStr());
    return true;
}

bool AnytimeSatClauseCommunicator::isDestructible() {
    if (_current_session) return false;
    for (auto& session : _cancelled_sessions) if (!session->isDestructible()) return false;
    return true;
}

void AnytimeSatClauseCommunicator::checkCertifiedUnsatReadyMsg() {

    if (_sent_cert_unsat_ready_msg || !_job->isInitialized()) return;

    int numExpectedReadyMsgs = 0;
    if (2*_job->getIndex()+1 < _job->getGlobalNumWorkers())
        numExpectedReadyMsgs++;
    if (2*_job->getIndex()+2 < _job->getGlobalNumWorkers())
        numExpectedReadyMsgs++;
    if (numExpectedReadyMsgs < _num_ready_msgs_from_children) return;

    _sent_cert_unsat_ready_msg = true;
    if (!_job->getJobTree().isRoot()) {
        LOG(V3_VERB, "sending comm ready msg\n");
        JobMessage msg(_job->getId(), _job->getRevision(), 0, MSG_NOTIFY_READY_FOR_PROOF_SAFE_SHARING);
        MyMpi::isend(_job->getJobTree().getParentNodeRank(), MSG_SEND_APPLICATION_MESSAGE, msg);
    } else {
        LOG(V3_VERB, "sharing enabled\n");
        if (!_proof_producer && !_msg_unsat_found.payload.empty()) {
            // A solver has already found UNSAT which was deferred then.
            // Now the message can be processed properly
            LOG(V3_VERB, "Now processing deferred UNSAT notification\n");
            MyMpi::isend(_job->getMyMpiRank(), MSG_SEND_APPLICATION_MESSAGE, _msg_unsat_found);
        }
    }
}

void AnytimeSatClauseCommunicator::setupProofProducer(JobMessage& msg) {

    ProofProducer::ProofSetup setup;
    setup.jobId = _job->getId();
    setup.revision = _job->getRevision();
    setup.finalEpoch = msg.epoch;
    setup.numWorkers = msg.payload[3];
    setup.threadsPerWorker = msg.payload[4];
    setup.thisJobNumThreads = _job->getNumThreads();
    setup.thisWorkerIndex = _job->getJobTree().getIndex();
    setup.winningInstance = msg.payload[0];
    memcpy(&setup.globalStartOfSuccessEpoch, msg.payload.data()+1, 2*sizeof(int));
    setup.solvingTime = _solving_time;
    setup.jobAgeSinceActivation = _job->getAgeSinceActivation();

    _proof_producer.reset(new ProofProducer(_params, setup, _job->getJobTree()));
}
