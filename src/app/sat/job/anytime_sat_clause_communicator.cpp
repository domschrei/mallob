
#include "anytime_sat_clause_communicator.hpp"

#include "app/sat/job/historic_clause_storage.hpp"
#include "comm/msgtags.h"
#include "util/logger.hpp"
#include "comm/mympi.hpp"
#include "app/sat/data/clause_comparison.hpp"
#include "util/sys/thread_pool.hpp"
#include "clause_sharing_session.hpp"
#include "base_sat_job.hpp"

void advanceCollective(BaseSatJob* job, JobMessage& msg, int broadcastTag) {
    if (job->getJobTree().isRoot() && msg.tag != broadcastTag) {
        // Self message: Switch from reduce to broadcast
        int oldTag = msg.tag;
        msg.tag = broadcastTag;
        job->getJobTree().sendToSelf(msg);
        msg.tag = oldTag;
    } else if (msg.tag == broadcastTag) {
        // Broadcast to children
        job->getJobTree().sendToAnyChildren(msg);
    } else {
        // Reduce to parent
        job->getJobTree().sendToParent(msg);
    }
}

AnytimeSatClauseCommunicator::AnytimeSatClauseCommunicator(const Parameters& params, BaseSatJob* job) : 
    _params(params), _job(job), 
    _cdb([&]() {
        AdaptiveClauseDatabase::Setup setup;
        setup.maxClauseLength = _params.strictClauseLengthLimit();
        setup.maxLbdPartitionedSize = _params.maxLbdPartitioningSize();
        setup.slotsForSumOfLengthAndLbd = _params.groupClausesByLengthLbdSum();
        setup.numLiterals = 0;
        return setup;
    }()),
    _cls_history(!params.collectClauseHistory() ? nullptr :
        new HistoricClauseStorage([&]() {
            AdaptiveClauseDatabase::Setup setup;
            setup.maxClauseLength = _params.strictClauseLengthLimit();
            setup.maxLbdPartitionedSize = _params.maxLbdPartitioningSize();
            setup.slotsForSumOfLengthAndLbd = _params.groupClausesByLengthLbdSum();
            setup.numLiterals = _job->getBufferLimit(MyMpi::size(MPI_COMM_WORLD), false);
            return setup;
        }(), _job)
    ),
    _sent_cert_unsat_ready_msg(!ClauseMetadata::enabled() && !params.deterministicSolving()) {

    _time_of_last_epoch_initiation = Timer::elapsedSecondsCached();
}

void AnytimeSatClauseCommunicator::communicate() {

    if (_proof_producer) _proof_producer->advanceFileMerger();

    // inactive?
    if (!_suspended && _job->getState() != ACTIVE) {
        // suspended!
        _suspended = true;
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
            _time_of_last_epoch_conclusion = Timer::elapsedSecondsCached();
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

    // Fetch result of clause history preparing a re-sharing
    if (_cls_history) {
        _cls_history->handleFinishedTasks();
    }
}

void AnytimeSatClauseCommunicator::handle(int source, int mpiTag, JobMessage& msg) {

    if (msg.returnedToSender) {
        // Message was sent by myself but was then returned.
        // Handle individual cases.
        LOG(V1_WARN, "%s : msg returned to sender\n", _job->toStr());

        if (msg.tag == MSG_INITIATE_CLAUSE_SHARING) {
            // Initiation of clause sharing was rejected:
            // go on without this child.
            if (_current_session) {
                _current_session->pruneChild(source);
            }
        }

        return;
    }

    assert(msg.jobId == _job->getId());
    if (handleClauseHistoryMessage(source, mpiTag, msg)) return;
    if (handleProofProductionMessage(source, mpiTag, msg)) return;
    if (handleClauseSharingMessage(source, mpiTag, msg)) return;
    assert(log_return_false("[ERROR] Unexpected job message!\n"));
}

bool AnytimeSatClauseCommunicator::handleClauseHistoryMessage(int source, int mpiTag, JobMessage& msg) {
    if (msg.tag == MSG_FORWARD_HISTORIC_CLAUSES) {
        int epochEnd = msg.payload.back();
        msg.payload.pop_back();
        _cls_history->handleIncomingMissingInterval(msg.epoch, epochEnd, std::move(msg.payload));
        return true;
    }
    if (msg.tag == MSG_REQUEST_HISTORIC_CLAUSES) {
        int epochBegin = msg.payload[0];
        int epochEnd = msg.payload[1];
        _cls_history->handleRequestMissingInterval(source == _job->getJobTree().getLeftChildNodeRank(), 
            epochBegin, epochEnd);
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
            _job->getJobTree().sendToSelf(msg);
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
        new ClauseSharingSession(_params, _job, _cdb, _cls_history.get(), _current_epoch, compensationFactor)
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

bool AnytimeSatClauseCommunicator::tryInitiateSharing() {

    if (_job->getState() != ACTIVE) return false;
    if (_proof_producer || !_sent_cert_unsat_ready_msg) return false;

    auto time = Timer::elapsedSecondsCached();
    bool nextEpochDue = _params.appCommPeriod() > 0 &&
        time - _time_of_last_epoch_initiation >= _params.appCommPeriod();
    if (_params.deterministicSolving()) {
        nextEpochDue &= _time_of_last_epoch_conclusion == 0 ||
            time - _time_of_last_epoch_conclusion >= _params.appCommPeriod();
    }
    if (!nextEpochDue) return false;

    bool lastEpochDone = !_current_session;
    if (!lastEpochDone) {
        if (!_params.deterministicSolving()) {
            // Warn that a new epoch is over-due, but only once for each skipped epoch ...
            int nbSkippedEpochs = (int) std::floor((time - _time_of_last_epoch_initiation) / _params.appCommPeriod()) - 1;
            if (nbSkippedEpochs > _last_skipped_epochs_warning) {
                LOG(V1_WARN, "[WARN] %s : Next epoch over-due -- %i periods skipped\n", _job->toStr(), nbSkippedEpochs);
                _last_skipped_epochs_warning = nbSkippedEpochs;
            }
        }
        return false;
    }

    _current_epoch++;
    _last_skipped_epochs_warning = 0;

    // Assemble job message
    JobMessage msg(_job->getId(), _job->getContextId(), _job->getRevision(), 
        _current_epoch, MSG_INITIATE_CLAUSE_SHARING);
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
    _job->getJobTree().sendToSelf(msg);
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
        JobMessage msg(_job->getId(), 0,
            _job->getRevision(), 0, MSG_NOTIFY_READY_FOR_PROOF_SAFE_SHARING);
        _job->getJobTree().sendToParent(msg);
    } else {
        LOG(V3_VERB, "sharing enabled\n");
        if (!_proof_producer && !_msg_unsat_found.payload.empty()) {
            // A solver has already found UNSAT which was deferred then.
            // Now the message can be processed properly
            LOG(V3_VERB, "Now processing deferred UNSAT notification\n");
            _job->getJobTree().sendToSelf(_msg_unsat_found);
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
