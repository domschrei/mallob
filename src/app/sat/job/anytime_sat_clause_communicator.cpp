
#include "anytime_sat_clause_communicator.hpp"

#include <assert.h>
#include <string.h>
#include <algorithm>
#include <cmath>
#include <utility>

#include "app/sat/data/clause_metadata.hpp"
#include "app/sat/job/historic_clause_storage.hpp"
#include "app/sat/job/inter_job_clause_sharer.hpp"
#include "comm/job_tree_snapshot.hpp"
#include "comm/msgtags.h"
#include "data/job_transfer.hpp"
#include "util/logger.hpp"
#include "comm/mympi.hpp"
#include "clause_sharing_session.hpp"
#include "base_sat_job.hpp"
#include "app/job_tree.hpp"
#include "comm/mpi_base.hpp"
#include "data/job_state.h"
#include "util/option.hpp"
#include "util/sys/timer.hpp"

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
    _cls_history(!params.collectClauseHistory() ? nullptr :
        new HistoricClauseStorage([&]() {
            AdaptiveClauseStore::Setup setup;
            setup.maxEffectiveClauseLength = _params.strictClauseLengthLimit()+ClauseMetadata::numInts();
            setup.maxLbdPartitionedSize = _params.maxLbdPartitioningSize();
            setup.slotsForSumOfLengthAndLbd = _params.groupClausesByLengthLbdSum();
            setup.numLiterals = _job->getBufferLimit(MyMpi::size(MPI_COMM_WORLD), false);
            return setup;
        }(), _job)
    ),
    _cross_job_clause_sharer(_job->getDescription().getGroupId() > 0 && _job->getJobTree().isRoot() ?
        new InterJobClauseSharer(_params, job->getDescription().getGroupId(), job->getContextId(), job->toStr()) : nullptr),
    _sent_cert_unsat_ready_msg(!params.proofOutputFile.isSet() && !params.deterministicSolving()) {

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
    if (_cross_sharing_session) {
        _cross_job_clause_sharer->setClauseBufferRevision(_job->getClausesRevision());
        _cross_sharing_session->advanceSharing();
        if (_cross_sharing_session->isDone()) {
            assert(_cross_job_clause_sharer->hasClausesToBroadcastInternally());
            JobMessage msg;
            msg.payload = _cross_job_clause_sharer->getClausesToBroadcastInternally();
            msg.treeIndexOfDestination = msg.treeIndexOfSender = 0;
            msg.contextIdOfDestination = msg.contextIdOfSender = _job->getActorContextId();
            advanceCollective(_job, msg, MSG_BROADCAST_CLAUSES_STATELESS);
            _cancelled_sessions.emplace_back(_cross_sharing_session.release());
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

    if (!_cross_job_clause_sharer && _job->getDescription().getGroupId() > 0 && _job->getJobTree().isRoot()) {
        _cross_job_clause_sharer.reset(new InterJobClauseSharer(_params,
            _job->getDescription().getGroupId(), _job->getContextId(), _job->toStr()));
        if (_current_session) _current_session->setAdditionalClauseListener([&](std::vector<int>& clauses) {
            feedLocalClausesIntoCrossSharing(clauses);
        });
    }

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
        if (msg.tag == MSG_INITIATE_CROSS_JOB_CLAUSE_SHARING) {
            if (_cross_sharing_session) {
                _cross_sharing_session->pruneChild(source);
            }
        }

        return;
    }

    if (handleClauseHistoryMessage(source, mpiTag, msg)) return;
    if (handleProofProductionMessage(source, mpiTag, msg)) return;
    if (handleClauseSharingMessage(source, mpiTag, msg)) return;

    if (msg.tag == MSG_BROADCAST_CLAUSES_STATELESS) {
        if (_job->getState() != ACTIVE) return;
        _job->getJobTree().sendToAnyChildren(msg);
        _job->digestSharingWithoutFilter(0, msg.payload, true);
        return;
    }

    assert(log_return_false("[ERROR] Unexpected job message mpitag=%i inttag=%i <= [%i]\n", mpiTag, msg.tag, source));
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
        initiateClauseSharing(msg, false);
        return true;
    }
    if (msg.tag == MSG_INITIATE_CROSS_JOB_CLAUSE_SHARING) {
        initiateCrossSharing(msg, false);
        return true;
    }

    // Advance all-reductions
    bool success = false;
    if (_current_session) {
        success = _current_session->advanceClauseAggregation(source, mpiTag, msg)
                || _current_session->advanceFilterAggregation(source, mpiTag, msg);
    }
    if (!success && _cross_sharing_session) {
        success = _cross_sharing_session->advanceClauseAggregation(source, mpiTag, msg)
                || _cross_sharing_session->advanceFilterAggregation(source, mpiTag, msg);
    }
    return success;
}

void AnytimeSatClauseCommunicator::initiateClauseSharing(JobMessage& msg, bool fromDeferredQueue) {

    if (_current_session || (!fromDeferredQueue && !_deferred_sharing_initiation_msgs.empty())) {
        // defer message until all past sessions are done
        // and all earlier deferred initiation messages have been processed
        LOG(V3_VERB, "%s : deferring CS initiation\n", _job->toStr());
        _deferred_sharing_initiation_msgs.push_back(std::move(msg));
        return;
    }

    // reject the clause sharing initiation if you are not active right now
    if (_job->getState() != ACTIVE && !_job->getJobTree().isRoot()) {
        msg.returnToSender(_job->getMyMpiRank(), MSG_SEND_APPLICATION_MESSAGE);
        return;
    }

    // no current sessions active - can start new session
    _current_epoch = msg.epoch;
    const auto snapshot = _job->getJobTree().getSnapshot();
    LOG(V4_VVER, "%s : INIT COMM e=%i nc=%i\n", _job->toStr(), _current_epoch, snapshot.nbChildren);

    // extract compensation factor for this session from the message
    float compensationFactor;
    static_assert(sizeof(float) == sizeof(int));
    memcpy(&compensationFactor, msg.payload.data(), sizeof(float));
    assert(compensationFactor >= 0.1 && compensationFactor <= 10);

    _current_session.reset(
        new ClauseSharingSession(_params, _job, snapshot, _cls_history.get(), _current_epoch, compensationFactor)
    );

    // register listener to grab final, filtered shared clauses and share them with other jobs
    if (_cross_job_clause_sharer) _current_session->setAdditionalClauseListener([&](std::vector<int>& clauses) {
        feedLocalClausesIntoCrossSharing(clauses);
    });

    // advance broadcast of initiation message
    msg.contextIdOfSender = snapshot.contextId;
    msg.treeIndexOfSender = snapshot.index;
    if (snapshot.leftChildContextId != 0) {
        msg.contextIdOfDestination = snapshot.leftChildContextId;
        msg.treeIndexOfDestination = snapshot.leftChildIndex;
        MyMpi::isend(snapshot.leftChildNodeRank, MSG_SEND_APPLICATION_MESSAGE, msg);
    }
    if (snapshot.rightChildContextId != 0) {
        msg.contextIdOfDestination = snapshot.rightChildContextId;
        msg.treeIndexOfDestination = snapshot.rightChildIndex;
        MyMpi::isend(snapshot.rightChildNodeRank, MSG_SEND_APPLICATION_MESSAGE, msg);
    }
}

void AnytimeSatClauseCommunicator::feedLocalClausesIntoCrossSharing(std::vector<int>& clauses) {
    _cross_job_clause_sharer->addInternalSharedClauses(clauses);
    auto& comm = _job->getGroupComm();
    if (comm.getCommSize() > 1 && comm.getMyLocalRank() == 0) {
        // build a cross-job clause sharing initiation message
        JobMessage msg;
        msg.tag = MSG_INITIATE_CROSS_JOB_CLAUSE_SHARING;
        auto packedComm = comm.serialize();
        msg.payload.resize(packedComm.size() / sizeof(int));
        memcpy(msg.payload.data(), packedComm.data(), packedComm.size());
        if (_cross_sharing_session) {
            // some cross-sharing session is still ongoing - defer
            _deferred_cross_sharing_initiation_msgs.push_back(std::move(msg));
        } else {
            // initiate!
            _job->getJobTree().sendToSelf(msg);
        }
    }
}

void AnytimeSatClauseCommunicator::initiateCrossSharing(JobMessage& msg, bool fromDeferredQueue) {

    if (_cross_sharing_session || !_cross_job_clause_sharer ||
            (!fromDeferredQueue && !_deferred_cross_sharing_initiation_msgs.empty())) {
        LOG(V3_VERB, "%s CROSSCOMM deferring initiation\n", _job->toStr());
        _deferred_cross_sharing_initiation_msgs.push_back(msg);
        return;
    }

    std::vector<uint8_t> bytesOfComm;
    bytesOfComm.resize(msg.payload.size() * sizeof(int));
    memcpy(bytesOfComm.data(), msg.payload.data(), bytesOfComm.size());
    auto comm = Serializable::get<GroupComm>(bytesOfComm);
    comm.localize(_job->getActorContextId());
    JobTreeSnapshot snapshot = comm.getTreeSnapshot();
    LOG(V4_VVER, "CROSSCOMM init: %s\n", comm.toStr().c_str());
    _cross_job_clause_sharer->updateCommunicator(comm.getCommSize(), comm.getMyLocalRank());
    _cross_sharing_session.reset(
        new ClauseSharingSession(_params, _cross_job_clause_sharer.get(), snapshot, nullptr, 0, 1)
    );
    msg.contextIdOfSender = snapshot.contextId;
    msg.treeIndexOfSender = snapshot.index;
    if (snapshot.leftChildNodeRank >= 0) {
        msg.contextIdOfDestination = snapshot.leftChildContextId;
        msg.treeIndexOfDestination = snapshot.leftChildIndex;
        MyMpi::isend(snapshot.leftChildNodeRank, MSG_SEND_APPLICATION_MESSAGE, msg);
    }
    if (snapshot.rightChildNodeRank >= 0) {
        msg.contextIdOfDestination = snapshot.rightChildContextId;
        msg.treeIndexOfDestination = snapshot.rightChildIndex;
        MyMpi::isend(snapshot.rightChildNodeRank, MSG_SEND_APPLICATION_MESSAGE, msg);
    }
    return;
}

void AnytimeSatClauseCommunicator::tryActivateDeferredSharingInitiation() {
    
    if (!_deferred_sharing_initiation_msgs.empty() && !_current_session) {
        // sessions are empty -> WILL succeed to initiate sharing
        // -> initiation message CAN be deleted afterwards.
        JobMessage msg = std::move(_deferred_sharing_initiation_msgs.front());
        _deferred_sharing_initiation_msgs.pop_front();
        initiateClauseSharing(msg, true);
    }

    if (!_deferred_cross_sharing_initiation_msgs.empty() && !_cross_sharing_session) {
        JobMessage msg = std::move(_deferred_cross_sharing_initiation_msgs.front());
        _deferred_cross_sharing_initiation_msgs.pop_front();
        initiateCrossSharing(msg, true);
    }
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
    LOG(V5_DEBG, "%s CS init\n", _job->toStr());
    return true;
}

bool AnytimeSatClauseCommunicator::isDestructible() {
    if (_current_session) return false;
    if (_cross_sharing_session) return false;
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
    if (_num_ready_msgs_from_children < numExpectedReadyMsgs) return;

    _sent_cert_unsat_ready_msg = true;
    if (!_job->getJobTree().isRoot()) {
        LOG(V3_VERB, "sending comm ready msg\n");
        JobMessage msg(_job->getId(), 0,
            _job->getRevision(), 0, MSG_NOTIFY_READY_FOR_PROOF_SAFE_SHARING);
        _job->getJobTree().sendToParent(msg);
    } else {
        LOG(V3_VERB, "sharing enabled (%i/%i ready)\n", _num_ready_msgs_from_children, numExpectedReadyMsgs);
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
