
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

    // inactive?
    if (!_suspended && _job->getState() != ACTIVE) {
        // suspended!
        _suspended = true;
        if (_use_cls_history) _cls_history.onSuspend();
        // cancel any active sessions, sending neutral element upwards
        for (auto& session : _sessions) {
            session.cancel();
        }
        return;
    }
    if (_suspended) {
        if (_job->getState() == ACTIVE) _suspended = false;
        else return;
    }

    // no communication at all?
    if (_params.appCommPeriod() <= 0) return;

    // clean up old sessions
    while (_sessions.size() > 1) {
        auto& session = _sessions.front();
        if (!session.isDestructible()) break;
        // can be deleted
        _sessions.pop_front();
    }

    // root: initiate sharing
    if (_job->getJobTree().isRoot() && tryInitiateSharing()) return;

    // any active sessions?
    if (_sessions.empty()) return;

    // Advance current sharing session
    auto& session = currentSession();
    if (session.isValid()) session.advanceSharing();
}

void AnytimeSatClauseCommunicator::handle(int source, int mpiTag, JobMessage& msg) {

    if (msg.jobId != _job->getId()) {
        LOG_ADD_SRC(V1_WARN, "[WARN] %s : stray job message meant for #%i\n", source, _job->toStr(), msg.jobId);
        return;
    }

    if (_job->getState() != ACTIVE) {
        // Not active any more: return message to sender
        if (!msg.returnedToSender) {
            msg.returnedToSender = true;
            MyMpi::isend(source, mpiTag, msg);
        }
        return;
    }

    if (msg.tag == ClauseHistory::MSG_CLAUSE_HISTORY_SEND_CLAUSES) {
        _cls_history.addEpoch(msg.epoch, msg.payload, /*entireIndex=*/true);
        _cls_history.sendNextBatches();
    }
    if (msg.tag == ClauseHistory::MSG_CLAUSE_HISTORY_SUBSCRIBE)
        _cls_history.onSubscribe(source, msg.payload[0], msg.payload[1]);
    if (msg.tag == ClauseHistory::MSG_CLAUSE_HISTORY_UNSUBSCRIBE)
        _cls_history.onUnsubscribe(source);

    // Process unsuccessful, returned messages
    if (msg.returnedToSender) {
        msg.returnedToSender = false;
        if (msg.tag == MSG_INITIATE_CLAUSE_SHARING) {
            // Initiation signal hit an inactive (?) child:
            // Pretend that it sent an empty set of clauses
            msg.tag = MSG_ALLREDUCE_CLAUSES;
            mpiTag = MSG_JOB_TREE_REDUCTION;
            msg.payload.resize(1);
            msg.payload[0] = 1; // num aggregated nodes
        } else if (msg.tag == MSG_ALLREDUCE_CLAUSES && mpiTag == MSG_JOB_TREE_BROADCAST) {
            // Distribution of clauses hit an inactive (?) child:
            // Pretend that it sent an empty filter
            msg.tag = MSG_ALLREDUCE_FILTER;
            mpiTag = MSG_JOB_TREE_REDUCTION;
            msg.payload.clear();
        } else return;
    }

    // Initial signal to initiate a sharing epoch
    if (msg.tag == MSG_INITIATE_CLAUSE_SHARING) {
        _current_epoch = msg.epoch;
        LOG(V5_DEBG, "%s : INIT COMM e=%i nc=%i\n", _job->toStr(), _current_epoch, 
            _job->getJobTree().getNumChildren());
        _sessions.emplace_back(_params, _job, _cdb, _cls_history, _current_epoch);
        if (!_job->hasPreparedSharing()) {
            int limit = _job->getBufferLimit(1, MyMpi::SELF);
            _job->prepareSharing(limit);
        }
        advanceCollective(_job, msg, MSG_INITIATE_CLAUSE_SHARING);
    }

    // Advance all-reductions
    bool success = false;
    if (!_sessions.empty()) {
        success = currentSession().advanceClauseAggregation(source, mpiTag, msg)
                || currentSession().advanceFilterAggregation(source, mpiTag, msg);
    }
    if (!success) {
        // Special case where clauses are broadcast but message was not processed:
        // Return an empty filter to the sender such that the sharing epoch may continue
        if (msg.tag == MSG_ALLREDUCE_CLAUSES && mpiTag == MSG_JOB_TREE_BROADCAST) {
            msg.payload.clear();
            msg.tag = MSG_ALLREDUCE_FILTER;
            MyMpi::isend(source, MSG_JOB_TREE_REDUCTION, msg);
        }
    }
}

bool AnytimeSatClauseCommunicator::tryInitiateSharing() {

    auto time = Timer::elapsedSecondsCached();
    bool nextEpochDue = time - _time_of_last_epoch_initiation >= _params.appCommPeriod();
    bool lastEpochDone = _sessions.empty() || currentSession().isConcluded();
    if (nextEpochDue && !lastEpochDone) {
        LOG(V1_WARN, "[WARN] %s : Next epoch over-due!\n", _job->toStr());
    }
    if (nextEpochDue && lastEpochDone) {
        _current_epoch++;
        JobMessage msg(_job->getId(), _job->getRevision(), _current_epoch, MSG_INITIATE_CLAUSE_SHARING);

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
    return false;
}

void AnytimeSatClauseCommunicator::feedHistoryIntoSolver() {
    if (_use_cls_history) _cls_history.feedHistoryIntoSolver();
}

bool AnytimeSatClauseCommunicator::isDestructible() {
    for (auto& session : _sessions) if (!session.isDestructible()) return false;
    return true;
}
