
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
    if (_job->getJobTree().isRoot() && 
            _time_of_last_epoch_conclusion > 0 &&
            Timer::elapsedSeconds() - _time_of_last_epoch_initiation >= _params.appCommPeriod()) {
        JobMessage msg;
        msg.jobId = _job->getId();
        _current_epoch++;
        msg.epoch = _current_epoch;
        msg.revision = _job->getRevision();
        msg.tag = MSG_INITIATE_CLAUSE_SHARING;
        _time_of_last_epoch_initiation = Timer::elapsedSeconds();
        _time_of_last_epoch_conclusion = 0;
        // Self message to initiate clause sharing
        MyMpi::isend(_job->getJobTree().getRank(), MSG_SEND_APPLICATION_MESSAGE, msg);
    }

    if (_sessions.empty()) return;
    auto& session = currentSession();

    // Enough clause buffers prepared / arrived?
    if (session._stage == PREPARING_CLAUSES) {
        // Must have received clauses from each existing child
        if (session._clause_buffers.size() >= session._desired_num_child_buffers) {
            // Done preparing sharing?
            if (_job->hasPreparedSharing()) {
                session.storePreparedClauseBuffer();
                session.initiateMergeOfClauseBuffers();
            }
        }
    }

    // Done merging?
    if (session._stage == MERGING && session._is_done_merging) {
        assert(session._clause_buffers_being_merged.size() == 1);
        session._stage = WAITING_FOR_CLAUSE_BCAST;
        session.publishMergedClauses();
    }

    // Done filtering?
    if (session._stage == PREPARING_FILTER && session._is_done_filtering) {
        assert(session._filter_future.valid());
        session._filter_future.get();
        session._is_done_filtering = false;
        LOG(V4_VVER, "%s : %i clauses in local filter section\n", _job->toStr(), _filter.size());
        session.addFilter(session._local_filter_bitset);
    }
}

void AnytimeSatClauseCommunicator::handle(int source, JobMessage& msg) {

    if (msg.jobId != _job->getId()) {
        LOG_ADD_SRC(V1_WARN, "[WARN] %s : stray job message meant for #%i\n", source, _job->toStr(), msg.jobId);
        return;
    }

    if (_job->getState() != ACTIVE) {
        // Not active any more: return message to sender
        if (!msg.returnedToSender) MyMpi::isend(source, MSG_RETURN_APPLICATION_MESSAGE, msg);
        return;
    }

    // Process unsuccessful, returned messages
    if (msg.returnedToSender) {
        msg.returnedToSender = false;
        if (_params.distributedDuplicateDetection() && msg.tag == MSG_DISTRIBUTE_CLAUSES) {
            // Distribution of clauses hit an inactive (?) child:
            // Pretend that it sent an empty filter
            msg.tag = MSG_GATHER_FILTER;
            msg.payload.clear();
        } else if (msg.tag == MSG_INITIATE_CLAUSE_SHARING) {
            // Initiation signal hit an inactive (?) child:
            // Pretend that it sent an empty set of clauses
            msg.tag = MSG_GATHER_CLAUSES;
            msg.payload.resize(1);
            msg.payload[0] = 1; // num aggregated nodes
        } else return;
    }
    
    // Discard messages from a revision "from the future"
    if (msg.revision > _job->getRevision()) return;

    if (msg.tag == ClauseHistory::MSG_CLAUSE_HISTORY_SEND_CLAUSES) {
        _cls_history.addEpoch(msg.epoch, msg.payload, /*entireIndex=*/true);
        learnClauses(msg.payload, msg.epoch, /*writeIntoClauseHistory=*/false);
    }
    if (msg.tag == ClauseHistory::MSG_CLAUSE_HISTORY_SUBSCRIBE)
        _cls_history.onSubscribe(source, msg.payload[0], msg.payload[1]);
    if (msg.tag == ClauseHistory::MSG_CLAUSE_HISTORY_UNSUBSCRIBE)
        _cls_history.onUnsubscribe(source);

    // discard old messages
    if (msg.epoch < _current_epoch) return;



    if (msg.tag == MSG_INITIATE_CLAUSE_SHARING) {
        _current_epoch = msg.epoch;
        _sessions.emplace_back(_params, _job, _cdb, _filter, _current_epoch);
        // TODO what if a job has to prepare for multiple sessions at once ... ?
        if (!_job->hasPreparedSharing()) {
            int limit = _job->getBufferLimit(1, MyMpi::SELF);
            _job->prepareSharing(limit);
        }
        advanceCollective(_job, msg, MSG_INITIATE_CLAUSE_SHARING);
        currentSession()._desired_num_child_buffers = _job->getJobTree().getNumChildren();
    }

    if (msg.tag == MSG_GATHER_CLAUSES) {
        // Gather received clauses, send to parent

        int numAggregated = msg.payload.back();
        msg.payload.pop_back();
        std::vector<int>& clauses = msg.payload;
        //testConsistency(clauses, getBufferLimit(numAggregated, BufferMode::ALL), /*sortByLbd=*/false);
        
        LOG(V5_DEBG, "%s : receive s=%i\n", _job->toStr(), clauses.size());
        
        // Add received clauses to local set of collected clauses
        currentSession()._clause_buffers.push_back(clauses);
        currentSession()._num_aggregated_nodes += numAggregated;
    }
    
    if (msg.tag == MSG_DISTRIBUTE_CLAUSES) {
        
        if (!_params.distributedDuplicateDetection()) {
            // Import the clauses and conclude this epoch
            learnClauses(msg.payload, msg.epoch, /*writeIntoClauseHistory=*/_use_cls_history);
            _time_of_last_epoch_conclusion = Timer::elapsedSeconds();
        }

        // Send clause buffer to children
        advanceCollective(_job, msg, MSG_DISTRIBUTE_CLAUSES);
        // Process clauses, begin filtering
        currentSession().processBroadcastClauses(msg.payload);
    }

    if (msg.tag == MSG_GATHER_FILTER) {
        currentSession().addFilter(msg.payload);
    }
    
    if (msg.tag == MSG_DISTRIBUTE_FILTER) {
        // Forward filter to children
        advanceCollective(_job, msg, MSG_DISTRIBUTE_FILTER);
        // Digest final clauses
        auto finalClauseBuffer = currentSession().applyGlobalFilter(msg.payload, currentSession()._clause_buffer_being_filtered);
        learnClauses(finalClauseBuffer, currentSession()._epoch, /*writeIntoClauseHistory=*/_use_cls_history);
        _time_of_last_epoch_conclusion = Timer::elapsedSeconds();
    }
}

void AnytimeSatClauseCommunicator::feedHistoryIntoSolver() {
    if (_use_cls_history) _cls_history.feedHistoryIntoSolver();
}







void AnytimeSatClauseCommunicator::Session::storePreparedClauseBuffer() {

    // +1 for local clauses
    _num_aggregated_nodes++;

    assert(_num_aggregated_nodes > 0);
    int totalSize = _job->getBufferLimit(_num_aggregated_nodes, MyMpi::ALL);
    int selfSize = _job->getBufferLimit(_num_aggregated_nodes, MyMpi::SELF);
    LOG(V5_DEBG, "%s : aggr=%i max_self=%i max_total=%i\n", _job->toStr(), 
            _num_aggregated_nodes, selfSize, totalSize);

    // Locally collect clauses from own solvers, add to clause buffer
    std::vector<int> selfClauses;
    // If not fully initialized yet, broadcast an empty set of clauses
    if (_job->getState() != ACTIVE || !_job->isInitialized() || !_job->hasPreparedSharing()) {
        selfClauses = std::vector<int>();
    } else {
        // Else, retrieve clauses from solvers
        LOG(V4_VVER, "%s : collect s<=%i\n", 
                    _job->toStr(), selfSize);
        Checksum checksum;
        selfClauses = _job->getPreparedClauses(checksum);
    }
    _clause_buffers.push_back(std::move(selfClauses));
}

void AnytimeSatClauseCommunicator::Session::initiateMergeOfClauseBuffers() {

    assert(!_is_done_merging);
    assert(_clause_buffers_being_merged.empty());

    _stage = MERGING;

    int numBuffersMerging = 0;
    for (auto& buffer : _clause_buffers) {
        _clause_buffers_being_merged.push_back(std::move(buffer));
        numBuffersMerging++;
        if (numBuffersMerging >= 4) break;
    }

    if (numBuffersMerging >= 4) {
        LOG(V1_WARN, "[WARN] %s : merging %i/%i local buffers\n", 
            _job->toStr(), numBuffersMerging, _clause_buffers.size());
    }

    // Remove merged clause buffers
    for (size_t i = 0; i < numBuffersMerging; i++) {
        _clause_buffers.pop_front();
    }
    
    // Merge all collected buffer into a single buffer
    int totalSize = _job->getBufferLimit(_num_aggregated_nodes, MyMpi::ALL);
    LOG(V4_VVER, "%s : merge n=%i s<=%i\n", 
                _job->toStr(), numBuffersMerging, totalSize);
    
    // Start asynchronous task to merge the clause buffers 
    _merge_future = ProcessWideThreadPool::get().addTask([this, totalSize]() {
        auto merger = _cdb.getBufferMerger(totalSize);
        for (auto& buffer : _clause_buffers_being_merged) {
            merger.add(_cdb.getBufferReader(buffer.data(), buffer.size()));
        }    
        std::vector<int> result = merger.merge(&_excess_clauses_from_merge);
        _clause_buffers_being_merged.resize(1);
        _clause_buffers_being_merged.front() = std::move(result);
        _is_done_merging = true;
    });
}

std::vector<int> AnytimeSatClauseCommunicator::Session::getMergedClauseBuffer() {

    assert(_merge_future.valid());
    _merge_future.get();
    _is_done_merging = false;

    assert(_clause_buffers_being_merged.size() == 1);

    std::vector<int> result = std::move(_clause_buffers_being_merged.front());
    _clause_buffers_being_merged.clear();

    // Some clauses may have been left behind during merge
    if (_excess_clauses_from_merge.size() > sizeof(size_t)/sizeof(int)) {
        // Add them as produced clauses to your local solver
        // so that they can be re-exported (if they are good enough)
        _job->returnClauses(_excess_clauses_from_merge);
    }

    return result;
}

void AnytimeSatClauseCommunicator::Session::publishMergedClauses() {

    // Send set of clauses to parent (possibly yourself, if root)
    JobMessage msg;
    msg.jobId = _job->getId();
    msg.revision = _job->getRevision();
    msg.epoch = _epoch;
    msg.tag = MSG_GATHER_CLAUSES;
    msg.payload = getMergedClauseBuffer();
    msg.payload.push_back(_num_aggregated_nodes);
    advanceCollective(_job, msg, MSG_DISTRIBUTE_CLAUSES);
    
    _num_aggregated_nodes = 0;
}

void AnytimeSatClauseCommunicator::Session::processBroadcastClauses(std::vector<int>& clauses) {

    // Filter the clauses
    assert(!_is_done_filtering);
    _clause_buffer_being_filtered = std::move(clauses);
    _stage = PREPARING_FILTER;
    _desired_num_child_buffers = _job->getJobTree().getNumChildren();
    _filter_future = ProcessWideThreadPool::get().addTask([&]() {
        
        auto reader = _cdb.getBufferReader(_clause_buffer_being_filtered.data(), _clause_buffer_being_filtered.size());
        auto clause = reader.getNextIncomingClause();
        _local_filter_bitset.clear();
        
        constexpr auto bitsPerElem = 8*sizeof(int);
        int shift = bitsPerElem;
        while (clause.begin != nullptr) {
            
            if (shift == bitsPerElem) {
                _local_filter_bitset.push_back(0);
                shift = 0;
            }
            
            if (!_filter.passClause(clause, _epoch)) {
                auto bitFiltered = 1 << shift;
                _local_filter_bitset.back() |= bitFiltered;
            }
            
            shift++;
            clause = reader.getNextIncomingClause();
        }

        _is_done_filtering = true;
    });
}

void AnytimeSatClauseCommunicator::Session::addFilter(std::vector<int>& filter) {
    
    LOG(V4_VVER, "adding filter\n");

    if (_aggregated_filter_bitset.size() < filter.size())
        _aggregated_filter_bitset.resize(filter.size());
    
    // Bitwise OR of local filter and aggregated filter, written into aggregated filter
    for (size_t i = 0; i < filter.size(); i++) {
        _aggregated_filter_bitset[i] |= filter[i];
    }
    _num_aggregated_filters++;

    // All necessary filters aggregated?
    int numDesiredFilters = 1 + _desired_num_child_buffers;
    if (_num_aggregated_filters >= numDesiredFilters) {
        // Can now publish the completed filter
        _stage = WAITING_FOR_FILTER_BCAST;
        publishLocalAggregatedFilter();
    }
}

void AnytimeSatClauseCommunicator::Session::publishLocalAggregatedFilter() {

    JobMessage msg;
    msg.jobId = _job->getId();
    msg.epoch = _epoch;
    msg.revision = _job->getRevision();
    msg.payload = std::move(_aggregated_filter_bitset);
    msg.tag = MSG_GATHER_FILTER;
    advanceCollective(_job, msg, MSG_DISTRIBUTE_FILTER);

    _num_aggregated_filters = 0;
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
