
#include "anytime_sat_clause_communicator.hpp"

#include "util/logger.hpp"
#include "comm/mympi.hpp"
#include "../sharing/filter/clause_filter.hpp"
#include "util/sys/thread_pool.hpp"

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
            session._allreduce_clauses.cancel();
            session._allreduce_filter.cancel();
        }
        return;
    }
    if (_suspended) {
        if (_job->getState() == ACTIVE) _suspended = false;
        else return;
    }

    // no communication at all?
    if (_params.appCommPeriod() <= 0) return;

    // update role in distributed filter
    //_filter.update(_job->getJobTree().getIndex(), _job->getVolume());

    // clean up old sessions
    while (_sessions.size() > 1) {
        auto& session = _sessions.front();
        if (!session.isDestructible()) break;
        // can be deleted
        _sessions.pop_front();
    }

    // root: initiate sharing
    if (_job->getJobTree().isRoot()) {
        auto time = Timer::elapsedSecondsCached();
        bool nextEpochDue = time - _time_of_last_epoch_initiation >= _params.appCommPeriod();
        bool lastEpochDone = _time_of_last_epoch_conclusion > 0;
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
            
            _time_of_last_epoch_conclusion = 0;
            
            // Self message to initiate clause sharing
            MyMpi::isend(_job->getJobTree().getRank(), MSG_SEND_APPLICATION_MESSAGE, msg);
            LOG(V4_VVER, "%s CS init\n", _job->toStr());
            return;
        }
    }

    if (_sessions.empty()) return;
    auto& session = currentSession();
    if (!session.isValid()) return;

    // Done preparing sharing?
    if (!session._allreduce_clauses.hasProducer() && _job->hasPreparedSharing()) {

        // Produce contribution to all-reduction of clauses
        LOG(V4_VVER, "%s CS produce cls\n", _job->toStr());
        session._allreduce_clauses.produce([&]() {
            Checksum checksum;
            auto clauses = _job->getPreparedClauses(checksum);
            clauses.push_back(1); // # aggregated workers
            return clauses;
        });
    
        // Calculate new sharing compensation factor from last sharing statistics
        auto [nbAdmitted, nbBroadcast] = _job->getLastAdmittedClauseShare();
        float admittedRatio = nbBroadcast == 0 ? 1 : ((float)nbAdmitted) / nbBroadcast;
        admittedRatio = std::max(0.01f, admittedRatio);
        float newCompensationFactor = std::max(1.f, std::min(
            (float)_params.clauseHistoryAggregationFactor(), 1.f/admittedRatio
        ));
        _compensation_factor = _compensation_decay * _compensation_factor + (1-_compensation_decay) * newCompensationFactor;
        _job->setSharingCompensationFactor(_compensation_factor);
        if (_job->getJobTree().isRoot()) {
            LOG(V3_VERB, "%s CS last sharing: %i/%i globally passed ~> c=%.3f\n", _job->toStr(), 
                nbAdmitted, nbBroadcast, _compensation_factor);       
        }
    
    } else if (!session._allreduce_clauses.hasProducer()) {
        // No sharing prepared yet: Retry
        _job->prepareSharing(_job->getBufferLimit(1, MyMpi::SELF));
    }
    
    // Advance all-reduction of clauses
    session._allreduce_clauses.advance();

    // All-reduction of clauses finished?
    if (session._allreduce_clauses.hasResult()) {

        LOG(V4_VVER, "%s CS filter\n", _job->toStr());

        // Some clauses may have been left behind during merge
        if (session._excess_clauses_from_merge.size() > sizeof(size_t)/sizeof(int)) {
            // Add them as produced clauses to your local solver
            // so that they can be re-exported (if they are good enough)
            _job->returnClauses(session._excess_clauses_from_merge);
        }

        // Fetch initial clause buffer (result of all-reduction of clauses)
        session._broadcast_clause_buffer = session._allreduce_clauses.extractResult();

        // Initiate production of local filter element for 2nd all-reduction 
        _job->filterSharing(session._broadcast_clause_buffer);
    }

    // Supply calculated local filter to the 2nd all-reduction
    if (!session._allreduce_filter.hasProducer() && _job->hasFilteredSharing()) {
        LOG(V4_VVER, "%s CS produce filter\n", _job->toStr());
        session._allreduce_filter.produce([&]() {return _job->getLocalFilter();});
    }

    // Advance all-reduction of filter
    session._allreduce_filter.advance();

    // All-reduction of clause filter finished?
    if (session._allreduce_filter.hasResult()) {
        
        LOG(V4_VVER, "%s CS apply filter\n", _job->toStr());

        // Extract and digest result
        auto filter = session._allreduce_filter.extractResult();
        _job->applyFilter(filter);
        if (_use_cls_history) {
            auto filteredClauses = session.applyGlobalFilter(filter, session._broadcast_clause_buffer);
            addToClauseHistory(filteredClauses, session._epoch);
        }

        // Conclude this sharing epoch
        _time_of_last_epoch_conclusion = Timer::elapsedSecondsCached();
    }
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
        _sessions.emplace_back(_params, _job, _cdb, _current_epoch);
        if (!_job->hasPreparedSharing()) {
            int limit = _job->getBufferLimit(1, MyMpi::SELF);
            _job->prepareSharing(limit);
        }
        advanceCollective(_job, msg, MSG_INITIATE_CLAUSE_SHARING);
    }

    // Advance all-reductions
    bool success = false;
    if (!_sessions.empty() && msg.tag == MSG_ALLREDUCE_CLAUSES && currentSession()._allreduce_clauses.isValid()) {
        success = currentSession()._allreduce_clauses.receive(source, mpiTag, msg);
        currentSession()._allreduce_clauses.advance();
    }
    if (!_sessions.empty() && msg.tag == MSG_ALLREDUCE_FILTER && currentSession()._allreduce_filter.isValid()) {
        success = currentSession()._allreduce_filter.receive(source, mpiTag, msg);
        currentSession()._allreduce_filter.advance();
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

void AnytimeSatClauseCommunicator::feedHistoryIntoSolver() {
    if (_use_cls_history) _cls_history.feedHistoryIntoSolver();
}

std::vector<int> AnytimeSatClauseCommunicator::Session::applyGlobalFilter(const std::vector<int>& filter, std::vector<int>& clauses) {

    size_t clsIdx = 0;
    size_t filterIdx = 0;
    constexpr auto bitsPerElem = 8*sizeof(int);
    auto reader = _cdb.getBufferReader(clauses.data(), clauses.size());
    auto writer = _cdb.getBufferBuilder();

    auto clause = reader.getNextIncomingClause();
    while (clause.begin != nullptr) {

        int filterInt = filter[filterIdx];
        auto bit = 1 << (clsIdx % bitsPerElem);
        if ((filterInt & bit) == 0) {
            // Clause passed
            writer.append(clause);
        }
        
        clsIdx++;
        if (clsIdx % bitsPerElem == 0) filterIdx++;
        clause = reader.getNextIncomingClause();
    }

    _num_broadcast_clauses = clsIdx;
    _num_admitted_clauses = writer.getNumAddedClauses();

    return writer.extractBuffer();
}

void AnytimeSatClauseCommunicator::addToClauseHistory(std::vector<int>& clauses, int epoch) {
    LOG(V4_VVER, "%s : learn s=%i\n", _job->toStr(), clauses.size());
    
    // Add clause batch to history
    _cls_history.addEpoch(epoch, clauses, /*entireIndex=*/false);

    // Send next batches of historic clauses to subscribers as necessary
    _cls_history.sendNextBatches();
}
