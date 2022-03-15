
#include "anytime_sat_clause_communicator.hpp"

#include "util/logger.hpp"
#include "comm/mympi.hpp"
#include "hordesat/utilities/clause_filter.hpp"
#include "util/sys/thread_pool.hpp"

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
    // no communication yet?
    if (Timer::elapsedSeconds() - _time_of_last_epoch_conclusion < _params.appCommPeriod()) return;

    // update role in distributed filter
    _filter.update(_job->getJobTree().getIndex(), _job->getVolume());

    // Prepare sharing: "Order" a buffer of clauses from the solver engine
    if (_stage == IDLE && !_job->hasPreparedSharing()) {
        int limit = getBufferLimit(_num_aggregated_nodes+1, MyMpi::SELF);
        _job->prepareSharing(limit);
        _stage = PREPARING_CLAUSES;
    }

    // Enough clause buffers prepared / arrived?
    if (_stage == PREPARING_CLAUSES) {
        size_t numChildren = 0;
        // Must have received clauses from each existing children
        if (_job->getJobTree().hasLeftChild()) numChildren++;
        if (_job->getJobTree().hasRightChild()) numChildren++;
        if (_clause_buffers.size() >= numChildren) {
            // Done preparing sharing?
            if (_job->hasPreparedSharing()) {
                // Able to perform a merge?
                if (_clause_buffers_being_merged.empty() && !_merge_future.valid()) {
                    _stage = MERGING;
                    initiateMergeOfClauseBuffers();
                }
            }
        }
    }

    // Done merging?
    if (_stage == MERGING && _is_done_merging) {
        assert(_clause_buffers_being_merged.size() == 1);
        _stage = WAITING_FOR_CLAUSE_BCAST;
        publishMergedClauses();
    }

    // Done filtering?
    if (_stage == PREPARING_FILTER && _is_done_filtering) {
        assert(_filter_future.valid());
        _filter_future.get();
        _is_done_filtering = false;
        LOG(V4_VVER, "%s : %i clauses in local filter section\n", _job->toStr(), _filter.size());
        addFilter(_local_filter_bitset);
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

    if (msg.returnedToSender) {
        if (_params.distributedDuplicateDetection() && msg.tag == MSG_DISTRIBUTE_CLAUSES) {
            // Distribution of clauses hit an inactive (?) child:
            // Pretend that it sent an empty filter
            msg.tag = MSG_GATHER_FILTER;
            msg.payload.clear();
        } else return;
    }
    
    // Discard messages from a revision "from the future"
    if (msg.revision > _job->getRevision()) return;

    if (_use_checksums) {
        Checksum chk;
        chk.combine(msg.jobId);
        for (int& lit : msg.payload) chk.combine(lit);
        if (chk.get() != msg.checksum.get()) {
            LOG(V1_WARN, "[WARN] %s : checksum fail in job msg (expected count: %ld, actual count: %ld)\n", 
                _job->toStr(), msg.checksum.count(), chk.count());
            return;
        }
    }

    if (msg.tag == ClauseHistory::MSG_CLAUSE_HISTORY_SEND_CLAUSES) {
        _cls_history.addEpoch(msg.epoch, msg.payload, /*entireIndex=*/true);
        learnClauses(msg.payload);
    }
    if (msg.tag == ClauseHistory::MSG_CLAUSE_HISTORY_SUBSCRIBE)
        _cls_history.onSubscribe(source, msg.payload[0], msg.payload[1]);
    if (msg.tag == ClauseHistory::MSG_CLAUSE_HISTORY_UNSUBSCRIBE)
        _cls_history.onUnsubscribe(source);

    if (msg.tag == MSG_GATHER_CLAUSES) {
        // Gather received clauses, send to parent

        int numAggregated = msg.payload.back();
        msg.payload.pop_back();
        std::vector<int>& clauses = msg.payload;
        //testConsistency(clauses, getBufferLimit(numAggregated, BufferMode::ALL), /*sortByLbd=*/false);
        
        LOG(V5_DEBG, "%s : receive s=%i\n", _job->toStr(), clauses.size());
        
        // Add received clauses to local set of collected clauses
        _clause_buffers.push_back(clauses);
        _num_aggregated_nodes += numAggregated;
    }
    
    if (msg.tag == MSG_DISTRIBUTE_CLAUSES) {
        _current_epoch = msg.epoch;

        // Learn received clauses, send them to children
        broadcastAndProcess(msg.payload);
    }

    if (msg.tag == MSG_GATHER_FILTER) {
        addFilter(msg.payload);
    }
    if (msg.tag == MSG_DISTRIBUTE_FILTER) {
        // Forward filter to children
        if (_job->getJobTree().hasLeftChild())
            MyMpi::isend(_job->getJobTree().getLeftChildNodeRank(), MSG_SEND_APPLICATION_MESSAGE, msg);
        if (_job->getJobTree().hasRightChild())
            MyMpi::isend(_job->getJobTree().getRightChildNodeRank(), MSG_SEND_APPLICATION_MESSAGE, msg);
        // Digest final clauses
        auto finalClauseBuffer = applyFilter(msg.payload, _clause_buffer_being_filtered);
        learnClauses(finalClauseBuffer);
    }
}

size_t AnytimeSatClauseCommunicator::getBufferLimit(int numAggregatedNodes, MyMpi::BufferQueryMode mode) {
    if (mode == MyMpi::SELF) return _clause_buf_base_size;
    return MyMpi::getBinaryTreeBufferLimit(numAggregatedNodes, _clause_buf_base_size, _clause_buf_discount_factor, mode);
}

std::vector<int> AnytimeSatClauseCommunicator::getMergedClauseBuffer() {

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

void AnytimeSatClauseCommunicator::publishMergedClauses() {

    // Merge all collected clauses, reset buffers
    std::vector<int> clausesToShare = getMergedClauseBuffer();

    if (_job->getJobTree().isRoot()) {
        // Rank zero: proceed to broadcast
        _current_epoch++;
        LOG(V3_VERB, "%s epoch %i: Broadcast clauses from %i sources\n", _job->toStr(), 
            _current_epoch, _num_aggregated_nodes);
        // Share complete set of clauses to children
        broadcastAndProcess(clausesToShare);
    } else {
        // Send set of clauses to parent
        int parentRank = _job->getJobTree().getParentNodeRank();
        JobMessage msg;
        msg.jobId = _job->getId();
        msg.revision = _job->getRevision();
        msg.epoch = 0; // unused
        msg.tag = MSG_GATHER_CLAUSES;
        msg.payload = clausesToShare;
        LOG_ADD_DEST(V4_VVER, "%s : gather s=%i", parentRank, _job->toStr(), msg.payload.size());
        msg.payload.push_back(_num_aggregated_nodes);
        if (_use_checksums) {
            msg.checksum.combine(msg.jobId);
            for (int& lit : msg.payload) msg.checksum.combine(lit);
        }
        MyMpi::isend(parentRank, MSG_SEND_APPLICATION_MESSAGE, msg);
    }

    _num_aggregated_nodes = 0;
}

void AnytimeSatClauseCommunicator::broadcastAndProcess(std::vector<int>& clauses) {

    sendClausesToChildren(clauses);

    if (!_params.distributedDuplicateDetection()) {
        // Just import the clauses and conclude this epoch
        learnClauses(clauses);
        _time_of_last_epoch_conclusion = Timer::elapsedSeconds();
        _stage = IDLE;
        return;
    }

    // Filter the clauses
    assert(!_is_done_filtering);
    _clause_buffer_being_filtered = std::move(clauses);
    _stage = PREPARING_FILTER;
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
            
            if (!_filter.passClause(clause, _current_epoch)) {
                auto bitFiltered = 1 << shift;
                _local_filter_bitset.back() |= bitFiltered;
            }
            
            shift++;
            clause = reader.getNextIncomingClause();
        }

        _is_done_filtering = true;
    });
}

void AnytimeSatClauseCommunicator::addFilter(std::vector<int>& filter) {
    
    LOG(V4_VVER, "adding filter\n");

    if (_aggregated_filter_bitset.size() < filter.size())
        _aggregated_filter_bitset.resize(filter.size());
    
    // Bitwise OR of local filter and aggregated filter, written into aggregated filter
    for (size_t i = 0; i < filter.size(); i++) {
        _aggregated_filter_bitset[i] |= filter[i];
    }
    _num_aggregated_filters++;

    // All necessary filters aggregated?
    int numDesiredFilters = 1;
    if (_job->getJobTree().hasLeftChild()) numDesiredFilters++;
    if (_job->getJobTree().hasRightChild()) numDesiredFilters++;
    if (_num_aggregated_filters == numDesiredFilters) {
        // Can now publish the completed filter
        _stage = WAITING_FOR_FILTER_BCAST;
        publishAggregatedFilter();
    }
}

void AnytimeSatClauseCommunicator::publishAggregatedFilter() {

    JobMessage msg;
    msg.jobId = _job->getId();
    msg.epoch = _current_epoch;
    msg.revision = _job->getRevision();
    msg.payload = std::move(_aggregated_filter_bitset);

    if (_job->getJobTree().isRoot()) {

        std::string str;
        for (size_t i = 0; i < msg.payload.size(); i++) {
            for (int shift = 0; shift < 32; shift++) {
                str += ((msg.payload[i] & (1 << shift)) == 0) ? "0" : "1";
            }
        }
        LOG(V4_VVER, "Filter: %s\n", str.c_str());

        // Broadcast filter
        msg.tag = MSG_DISTRIBUTE_FILTER;
        if (_job->getJobTree().hasLeftChild())
            MyMpi::isend(_job->getJobTree().getLeftChildNodeRank(), MSG_SEND_APPLICATION_MESSAGE, msg);
        if (_job->getJobTree().hasRightChild())
            MyMpi::isend(_job->getJobTree().getRightChildNodeRank(), MSG_SEND_APPLICATION_MESSAGE, msg);
        // Digest clauses with filter applied
        auto finalClauseBuffer = applyFilter(msg.payload, _clause_buffer_being_filtered);
        learnClauses(finalClauseBuffer);
    } else {
        // Send filter upwards
        msg.tag = MSG_GATHER_FILTER;
        int dest = _job->getJobTree().getParentNodeRank();
        MyMpi::isend(dest, MSG_SEND_APPLICATION_MESSAGE, msg);
    }

    _num_aggregated_filters = 0;
}

std::vector<int> AnytimeSatClauseCommunicator::applyFilter(const std::vector<int>& filter, std::vector<int>& clauses) {

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

    // Conclude this sharing epoch
    _time_of_last_epoch_conclusion = Timer::elapsedSeconds();
    _stage = IDLE;

    return writer.extractBuffer();
}

void AnytimeSatClauseCommunicator::learnClauses(std::vector<int>& clauses) {
    LOG(V4_VVER, "%s : learn s=%i\n", _job->toStr(), clauses.size());
    
    if (clauses.size() > 0) {
        // Locally learn clauses
        
        // If not active or not fully initialized yet: discard clauses
        if (_job->getState() != ACTIVE || !_job->isInitialized()) {
            LOG(V4_VVER, "%s : discard buffer, job is not (yet?) active\n", 
                    _job->toStr());
            return;
        }

        // Compute checksum for this set of clauses
        Checksum checksum;
        if (_use_checksums) {
            checksum.combine(_job->getId());
            for (int lit : clauses) checksum.combine(lit);
        }

        // Locally digest clauses
        LOG(V4_VVER, "%s : digest\n", _job->toStr());
        _job->digestSharing(clauses, checksum);
        LOG(V4_VVER, "%s : digested\n", _job->toStr());
    }
}

void AnytimeSatClauseCommunicator::sendClausesToChildren(std::vector<int>& clauses) {
    
    // Send clauses to children
    JobMessage msg;
    msg.jobId = _job->getId();
    msg.revision = _job->getRevision();
    msg.epoch = _current_epoch;
    msg.tag = MSG_DISTRIBUTE_CLAUSES;
    msg.payload = clauses;

    if (_use_checksums) {
        msg.checksum.combine(msg.jobId);
        for (int& lit : msg.payload) msg.checksum.combine(lit);
    }

    int childRank;
    if (_job->getJobTree().hasLeftChild()) {
        childRank = _job->getJobTree().getLeftChildNodeRank();
        LOG_ADD_DEST(V4_VVER, "%s : broadcast s=%i", childRank, _job->toStr(), msg.payload.size());
        MyMpi::isend(childRank, MSG_SEND_APPLICATION_MESSAGE, msg);
    }
    if (_job->getJobTree().hasRightChild()) {
        childRank = _job->getJobTree().getRightChildNodeRank();
        LOG_ADD_DEST(V4_VVER, "%s : broadcast s=%i", childRank, _job->toStr(), msg.payload.size());
        MyMpi::isend(childRank, MSG_SEND_APPLICATION_MESSAGE, msg);
    }

    if (_use_cls_history) {
        // Add clause batch to history
        _cls_history.addEpoch(msg.epoch, msg.payload, /*entireIndex=*/false);

        // Send next batches of historic clauses to subscribers as necessary
        _cls_history.sendNextBatches();
    }
}

void AnytimeSatClauseCommunicator::initiateMergeOfClauseBuffers() {

    // +1 for local clauses, but at most as many contributions as there are nodes
    _num_aggregated_nodes = std::min(_num_aggregated_nodes+1, _job->getJobTree().getCommSize());

    assert(_num_aggregated_nodes > 0);
    int totalSize = getBufferLimit(_num_aggregated_nodes, MyMpi::ALL);
    int selfSize = getBufferLimit(_num_aggregated_nodes, MyMpi::SELF);
    LOG(V5_DEBG, "%s : aggr=%i max_self=%i max_total=%i\n", _job->toStr(), 
            _num_aggregated_nodes, selfSize, totalSize);

    // Locally collect clauses from own solvers, add to clause buffer
    std::vector<int> selfClauses;
    // If not fully initialized yet, broadcast an empty set of clauses
    if (_job->getState() != ACTIVE || !_job->isInitialized() || !_job->hasPreparedSharing()) {
        selfClauses = getEmptyBuffer();
    } else {
        // Else, retrieve clauses from solvers
        LOG(V4_VVER, "%s : collect s<=%i\n", 
                    _job->toStr(), selfSize);
        Checksum checksum;
        selfClauses = _job->getPreparedClauses(checksum);
        if (_use_checksums) {
            // Verify checksum of clause buffer from solver backend
            Checksum chk;
            chk.combine(_job->getId());
            for (int lit : selfClauses) chk.combine(lit);
            if (checksum.get() != chk.get()) {
                LOG(V1_WARN, "[WARN] %s : checksum fail in clsbuf (expected count: %ld, actual count: %ld)\n", 
                _job->toStr(), checksum.count(), chk.count());
                selfClauses = getEmptyBuffer();
            }
        }
        //testConsistency(selfClauses, 0 /*do not check buffer's size limit*/, /*sortByLbd=*/_sort_by_lbd);
    }
    _clause_buffers.push_back(std::move(selfClauses));

    assert(!_is_done_merging);
    assert(_clause_buffers_being_merged.empty());

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
    LOG(V4_VVER, "%s : merge n=%i s<=%i\n", 
                _job->toStr(), numBuffersMerging, totalSize);
    
    // Start asynchronous task to merge the clause buffers 
    assert(_stage == MERGING);
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

void AnytimeSatClauseCommunicator::feedHistoryIntoSolver() {
    if (_use_cls_history) _cls_history.feedHistoryIntoSolver();
}
