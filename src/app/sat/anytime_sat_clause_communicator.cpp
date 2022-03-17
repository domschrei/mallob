
#include "anytime_sat_clause_communicator.hpp"

#include "util/logger.hpp"
#include "comm/mympi.hpp"
#include "hordesat/utilities/clause_filter.hpp"
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
    _filter.update(_job->getJobTree().getIndex(), _job->getVolume());

    // clean up old sessions
    while (_sessions.size() > 1) {
        auto& session = _sessions.front();
        if (!session.isDestructible()) break;
        // can be deleted
        _sessions.pop_front();
    }

    // root: initiate sharing
    if (_job->getJobTree().isRoot()) {
        bool nextEpochDue = Timer::elapsedSeconds() - _time_of_last_epoch_initiation >= _params.appCommPeriod();
        bool lastEpochDone = _time_of_last_epoch_conclusion > 0;
        if (nextEpochDue && !lastEpochDone) {
            LOG(V1_WARN, "[WARN] %s : Next epoch over-due!\n", _job->toStr());
        }
        if (nextEpochDue && lastEpochDone) {
            _current_epoch++;
            JobMessage msg(_job->getId(), _job->getRevision(), _current_epoch, MSG_INITIATE_CLAUSE_SHARING);
            _time_of_last_epoch_initiation = Timer::elapsedSeconds();
            _time_of_last_epoch_conclusion = 0;
            // Self message to initiate clause sharing
            MyMpi::isend(_job->getJobTree().getRank(), MSG_SEND_APPLICATION_MESSAGE, msg);
            return;
        }
    }

    if (_sessions.empty()) return;
    auto& session = currentSession();
    if (!session.isValid()) return;

    // Done preparing sharing?
    if (_job->hasPreparedSharing()) {
        // Produce contribution to all-reduction of clauses
        session._allreduce_clauses.produce([&]() {
            Checksum checksum;
            return _job->getPreparedClauses(checksum);
        });
    } else if (!session._allreduce_clauses.hasProducer()) {
        // No sharing prepared yet: Retry
        _job->prepareSharing(_job->getBufferLimit(1, MyMpi::SELF));
    }
    
    // Advance all-reduction of clauses
    session._allreduce_clauses.advance();

    // All-reduction of clauses finished?
    if (session._allreduce_clauses.hasResult()) {

        // Some clauses may have been left behind during merge
        if (session._excess_clauses_from_merge.size() > sizeof(size_t)/sizeof(int)) {
            // Add them as produced clauses to your local solver
            // so that they can be re-exported (if they are good enough)
            _job->returnClauses(session._excess_clauses_from_merge);
        }

        // Fetch initial clause buffer (result of all-reduction of clauses)
        session._broadcast_clause_buffer = session._allreduce_clauses.extractResult();
        
        // Initiate production of local filter element for 2nd all-reduction 
        session._allreduce_filter.produce([&, clauseBuffer = &session._broadcast_clause_buffer]() {
            
            std::vector<int> bitset; // filter will be written into this object
            auto reader = _cdb.getBufferReader(clauseBuffer->data(), clauseBuffer->size());
            
            // Iterate over all clauses and mark the ones which do not pass the filter
            constexpr auto bitsPerElem = 8*sizeof(int);
            int shift = bitsPerElem;
            auto clause = reader.getNextIncomingClause();
            while (clause.begin != nullptr) {
                
                if (shift == bitsPerElem) {
                    bitset.push_back(0);
                    shift = 0;
                }
                
                if (!_filter.passClause(clause, session._epoch)) {
                    auto bitFiltered = 1 << shift;
                    bitset.back() |= bitFiltered;
                }
                
                shift++;
                clause = reader.getNextIncomingClause();
            }

            return bitset;
        });
    }

    // Advance all-reduction of filter
    session._allreduce_filter.advance();

    // All-reduction of clause filter finished?
    if (session._allreduce_filter.hasResult()) {
        
        // Extract and digest result
        auto filter = session._allreduce_filter.extractResult();
        auto clausesToLearn = session.applyGlobalFilter(filter, session._broadcast_clause_buffer);
        learnClauses(clausesToLearn, session._epoch, /*writeIntoClauseHistory=*/_use_cls_history);

        // Conclude this sharing epoch
        _time_of_last_epoch_conclusion = Timer::elapsedSeconds();
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
        learnClauses(msg.payload, msg.epoch, /*writeIntoClauseHistory=*/false);
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
        _sessions.emplace_back(_params, _job, _cdb, _filter, _current_epoch);
        if (!_job->hasPreparedSharing()) {
            int limit = _job->getBufferLimit(1, MyMpi::SELF);
            _job->prepareSharing(limit);
        }
        advanceCollective(_job, msg, MSG_INITIATE_CLAUSE_SHARING);
    }

    // Advance all-reductions
    bool success;
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

    if (_job->getJobTree().isRoot()) {
        LOG(V3_VERB, "%s : %i/%i passed global filter\n", _job->toStr(), writer.getNumAddedClauses(), clsIdx);       
    } else {
        LOG(V4_VVER, "%s : %i/%i passed global filter\n", _job->toStr(), writer.getNumAddedClauses(), clsIdx);
    }

    return writer.extractBuffer();
}

void AnytimeSatClauseCommunicator::learnClauses(std::vector<int>& clauses, int epoch, bool writeIntoClauseHistory) {
    LOG(V4_VVER, "%s : learn s=%i\n", _job->toStr(), clauses.size());
    
    if (clauses.size() > 0) {
        // Locally learn clauses
        
        // If not active or not fully initialized yet: discard clauses
        if (_job->getState() != ACTIVE || !_job->isInitialized()) {
            LOG(V4_VVER, "%s : discard buffer, job is not (yet?) active\n", 
                    _job->toStr());
            return;
        }

        // Locally digest clauses
        LOG(V4_VVER, "%s : digest\n", _job->toStr());
        Checksum checksum;
        _job->digestSharing(clauses, checksum);
        LOG(V4_VVER, "%s : digested\n", _job->toStr());
    }

    if (writeIntoClauseHistory) {
        // Add clause batch to history
        _cls_history.addEpoch(epoch, clauses, /*entireIndex=*/false);

        // Send next batches of historic clauses to subscribers as necessary
        _cls_history.sendNextBatches();
    }
}
