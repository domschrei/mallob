
#include "anytime_sat_clause_communicator.hpp"

#include "util/logger.hpp"
#include "comm/mympi.hpp"
#include "hordesat/utilities/clause_filter.hpp"
#include "util/sys/thread_pool.hpp"

void AnytimeSatClauseCommunicator::handle(int source, JobMessage& msg) {

    if (msg.jobId != _job->getId()) {
        LOG_ADD_SRC(V1_WARN, "[WARN] %s : stray job message meant for #%i\n", source, _job->toStr(), msg.jobId);
        return;
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

    } else if (msg.tag == MSG_DISTRIBUTE_CLAUSES) {
        _current_epoch = msg.epoch;

        // Learn received clauses, send them to children
        broadcastAndLearn(msg.payload);
    }
}

size_t AnytimeSatClauseCommunicator::getBufferLimit(int numAggregatedNodes, MyMpi::BufferQueryMode mode) {
    if (mode == MyMpi::SELF) return _clause_buf_base_size;
    return MyMpi::getBinaryTreeBufferLimit(numAggregatedNodes, _clause_buf_base_size, _clause_buf_discount_factor, mode);
}

bool AnytimeSatClauseCommunicator::canSendClauses() {
    if (!_initialized || _job->getState() != ACTIVE) return false;

    // Done merging?
    if (_is_merging && _is_done_merging) {
        assert(_clause_buffers_being_merged.size() == 1);
        return true;
    }
    
    // Prepare sharing: "Order" a buffer of clauses from the solver engine
    if (!_job->hasPreparedSharing()) {
        int limit = getBufferLimit(_num_aggregated_nodes+1, MyMpi::SELF);
        _job->prepareSharing(limit);
    }

    size_t numChildren = 0;
    // Must have received clauses from each existing children
    if (_job->getJobTree().hasLeftChild()) numChildren++;
    if (_job->getJobTree().hasRightChild()) numChildren++;
    if (_clause_buffers.size() >= numChildren) {
        // Done preparing sharing?
        if (_job->hasPreparedSharing()) {
            // Able to perform a merge?
            if (!_is_merging && _clause_buffers_being_merged.empty()) 
                initiateMergeOfClauseBuffers();
        }
    }

    return false;
}

std::vector<int> AnytimeSatClauseCommunicator::getMergedClauseBuffer() {

    assert(_merge_future.valid());
    _merge_future.get();
    _is_merging = false;
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

void AnytimeSatClauseCommunicator::sendClausesToParent() {

    // Merge all collected clauses, reset buffers
    std::vector<int> clausesToShare = getMergedClauseBuffer();

    if (_job->getJobTree().isRoot()) {
        // Rank zero: proceed to broadcast
        LOG(V3_VERB, "%s epoch %i: Broadcast clauses from %i sources\n", _job->toStr(), 
            _current_epoch, _num_aggregated_nodes);
        // Share complete set of clauses to children
        broadcastAndLearn(clausesToShare);
        _current_epoch++;
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
    _job->resetLastCommTime();
}

void AnytimeSatClauseCommunicator::broadcastAndLearn(std::vector<int>& clauses) {
    //testConsistency(clauses, 0, /*sortByLbd=*/false);
    sendClausesToChildren(clauses);
    learnClauses(clauses);
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

    assert(!_is_merging);
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
    _is_merging = true;
    _merge_future = ProcessWideThreadPool::get().addTask([this, totalSize]() {
        auto merger = _cdb.getBufferMerger();
        for (auto& buffer : _clause_buffers_being_merged) {
            merger.add(_cdb.getBufferReader(buffer.data(), buffer.size()));
        }    
        std::vector<int> result = merger.merge(totalSize, &_excess_clauses_from_merge);
        _clause_buffers_being_merged.resize(1);
        _clause_buffers_being_merged.front() = std::move(result);
        _is_done_merging = true;
    });
}

void AnytimeSatClauseCommunicator::suspend() {
    if (_use_cls_history) _cls_history.onSuspend();
}

bool AnytimeSatClauseCommunicator::testConsistency(std::vector<int>& buffer, size_t maxSize, bool sortByLbd) {
    if (buffer.empty()) return true;

    if (maxSize > 0 && buffer.size() > maxSize) {
        LOG(V0_CRIT, "[ERROR] Clause buffer too full (%i/%i)\n", buffer.size(), maxSize);
        Logger::getMainInstance().flush();
        abort();
    }

    int consistent = 0;
    size_t pos = 0;

    int numVips = buffer[pos++];
    int countedVips = 0;
    int clslength = 0;
    while (countedVips < numVips) {
        if (pos >= buffer.size()) {
            consistent = 1; break;
        }
        if (buffer[pos++] == 0) {
            if (clslength <= 0) {
                consistent = 2; break;
            }
            countedVips++;
            clslength = 0;
        }
        else clslength++;
    }
    if (countedVips != numVips) {
        consistent = 3;
    }

    int length = 1;
    while (consistent == 0 && pos < buffer.size()) {
        int numCls = buffer[pos];
        if (numCls < 0) {
            consistent = 4; break;
        }

        if (length > 1 && sortByLbd && numCls > 1) {
            
            // Sort indices of clauses ascending by LBD score
            std::vector<int> clsIndices(numCls);
            for (size_t i = 0; i < numCls; i++) clsIndices[i] = i;
            std::sort(clsIndices.begin(), clsIndices.end(), [&buffer, pos, length](const int& a, const int& b) {
                // Compare LBD score of a-th clause with LBD score of b-th clause
                return buffer[pos+1+a*length] < buffer[pos+1+b*length];
            });

            // Store originally sorted clauses in a temporary vector
            std::vector<int> originalClauses(buffer.begin()+pos+1, buffer.begin()+pos+1+numCls*length);

            // Write clauses according to their correct ordering into the buffer
            size_t i = pos+1;
            int currentLbd = 0;
            for (int clsIndex : clsIndices) {
                assert(currentLbd <= originalClauses[clsIndex*length] 
                    || LOG_RETURN_FALSE("%i %i %i\n", length, currentLbd, originalClauses[clsIndex*length]));
                currentLbd = originalClauses[clsIndex*length];
                assert(currentLbd >= 2);
                // For each clause literal:
                for (size_t x = 0; x < length; x++) {
                    buffer[i++] = originalClauses[clsIndex*length + x];
                }
            }
            assert(i == pos+1+numCls*length);
        }

        for (int offset = 1; offset <= numCls * length; offset++) {
            if (pos+offset >= buffer.size()) {
                consistent = 5; break;
            }
            int lit = buffer[pos+offset];
            if (lit == 0) {
                consistent = 6; break;
            }
        }
        if (consistent != 0) break;
        pos += numCls * length + 1;
        length++;
    }

    if (consistent > 0) {
        LOG(V0_CRIT, "[ERROR] Inconsistency %i in clause buffer at position %i: \n", consistent, pos);
        for (size_t p = 0; p < buffer.size(); p++) {
            if (p == pos) LOG_OMIT_PREFIX(V0_CRIT, "(%i) ", buffer[p]);
            else          LOG_OMIT_PREFIX(V0_CRIT, "%i ", buffer[p]);
        }
        LOG_OMIT_PREFIX(V0_CRIT, "\n");
        Logger::getMainInstance().flush();
        abort();
    }
    return consistent == 0;
}

void AnytimeSatClauseCommunicator::feedHistoryIntoSolver() {
    if (_use_cls_history) _cls_history.feedHistoryIntoSolver();
}
