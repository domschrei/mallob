
#include "anytime_sat_clause_communicator.hpp"

#include "util/logger.hpp"
#include "comm/mympi.hpp"
#include "hordesat/utilities/clause_filter.hpp"

size_t AnytimeSatClauseCommunicator::getBufferLimit(int numAggregatedNodes, BufferMode mode) {
    float limit = _clause_buf_base_size * std::pow(_clause_buf_discount_factor, std::log2(numAggregatedNodes+1));
    if (mode == SELF) {
        return std::ceil(limit);
    } else {
        return std::ceil(numAggregatedNodes * limit);
    }
}

bool AnytimeSatClauseCommunicator::canSendClauses() {
    if (!_initialized || _job->getState() != ACTIVE) return false;

    size_t numChildren = 0;
    // Must have received clauses from each existing children
    if (_job->getJobTree().hasLeftChild()) numChildren++;
    if (_job->getJobTree().hasRightChild()) numChildren++;

    if (_clause_buffers.size() >= numChildren) {
        if (!_job->hasPreparedSharing()) {
            int limit = getBufferLimit(_num_aggregated_nodes+1, BufferMode::SELF);
            _job->prepareSharing(limit);
        }
        if (_job->hasPreparedSharing()) {
            return true;
        }
    }

    return false;
}

void AnytimeSatClauseCommunicator::sendClausesToParent() {

    // Merge all collected clauses, reset buffers
    std::vector<int> clausesToShare = prepareClauses();

    if (_job->getJobTree().isRoot()) {
        // Share complete set of clauses to children
        log(V4_VVER, "%s : switch gather => broadcast\n", _job->toStr()); 
        learnClauses(clausesToShare);
        sendClausesToChildren(clausesToShare);
    } else {
        // Send set of clauses to parent
        int parentRank = _job->getJobTree().getParentNodeRank();
        JobMessage msg;
        msg.jobId = _job->getId();
        msg.epoch = 0; // unused
        msg.tag = MSG_GATHER_CLAUSES;
        msg.payload = clausesToShare;
        msg.payload.push_back(_num_aggregated_nodes);
        log(LOG_ADD_DESTRANK | V4_VVER, "%s : gather", parentRank, _job->toStr());
        MyMpi::isend(MPI_COMM_WORLD, parentRank, MSG_SEND_APPLICATION_MESSAGE, msg);
    }

    _num_aggregated_nodes = 0;
}

void AnytimeSatClauseCommunicator::handle(int source, JobMessage& msg) {

    if (msg.tag == MSG_GATHER_CLAUSES) {
        // Gather received clauses, send to parent
        // TODO count each child only once
        int numAggregated = msg.payload.back();
        msg.payload.pop_back();
        std::vector<int>& clauses = msg.payload;
        testConsistency(clauses, getBufferLimit(numAggregated, BufferMode::ALL));
        
        log(V5_DEBG, "%s : receive, size %i\n", _job->toStr(), clauses.size());
        
        // Add received clauses to local set of collected clauses
        _clause_buffers.push_back(clauses);
        _num_aggregated_nodes += numAggregated;

        if (canSendClauses()) sendClausesToParent();

    } else if (msg.tag == MSG_DISTRIBUTE_CLAUSES) {
        // Learn received clauses, send them to children
        std::vector<int>& clauses = msg.payload;
        learnClauses(clauses);
        sendClausesToChildren(clauses);
    }
}

void AnytimeSatClauseCommunicator::learnClauses(std::vector<int>& clauses) {
    log(V4_VVER, "%s : learn, size %i\n", _job->toStr(), clauses.size());
    testConsistency(clauses, 0);
    
    if (clauses.size() > 0) {
        // Locally learn clauses
        
        // If not active or not fully initialized yet: discard clauses
        if (_job->getState() != ACTIVE || !_job->isInitialized()) {
            log(V4_VVER, "%s : discard buffer, job is not (yet?) active\n", 
                    _job->toStr());
            return;
        }

        // Locally digest clauses
        log(V4_VVER, "%s : digest\n", _job->toStr());
        _job->digestSharing(clauses);
        log(V4_VVER, "%s : digested\n", _job->toStr());
    }
}

void AnytimeSatClauseCommunicator::sendClausesToChildren(const std::vector<int>& clauses) {
    
    // Send clauses to children
    JobMessage msg;
    msg.jobId = _job->getId();
    msg.epoch = 0; // unused
    msg.tag = MSG_DISTRIBUTE_CLAUSES;
    msg.payload = clauses;
    int childRank;
    if (_job->getJobTree().hasLeftChild()) {
        childRank = _job->getJobTree().getLeftChildNodeRank();
        log(LOG_ADD_DESTRANK | V4_VVER, "%s : broadcast", childRank, _job->toStr());
        MyMpi::isend(MPI_COMM_WORLD, childRank, MSG_SEND_APPLICATION_MESSAGE, msg);
    }
    if (_job->getJobTree().hasRightChild()) {
        childRank = _job->getJobTree().getRightChildNodeRank();
        log(LOG_ADD_DESTRANK | V4_VVER, "%s : broadcast", childRank, _job->toStr());
        MyMpi::isend(MPI_COMM_WORLD, childRank, MSG_SEND_APPLICATION_MESSAGE, msg);
    }
}

std::vector<int> AnytimeSatClauseCommunicator::prepareClauses() {

    // +1 for local clauses
    _num_aggregated_nodes++;

    assert(_num_aggregated_nodes > 0);
    int totalSize = getBufferLimit(_num_aggregated_nodes, BufferMode::ALL);
    int selfSize = getBufferLimit(_num_aggregated_nodes, BufferMode::SELF);
    log(V5_DEBG, "%s : aggregated=%i max_self=%i max_total=%i\n", _job->toStr(), 
            _num_aggregated_nodes, selfSize, totalSize);

    // Locally collect clauses from own solvers, add to clause buffer
    std::vector<int> selfClauses;
    // If not fully initialized yet, broadcast an empty set of clauses
    if (_job->getState() != ACTIVE || !_job->isInitialized() || !_job->hasPreparedSharing()) {
        selfClauses = std::vector<int>();
    } else {
        // Else, retrieve clauses from solvers
        log(V4_VVER, "%s : collect local cls, max. size %i\n", 
                    _job->toStr(), selfSize);
        selfClauses = _job->getPreparedClauses();
        testConsistency(selfClauses, selfSize);
    }
    _clause_buffers.push_back(selfClauses);

    // Merge all collected buffer into a single buffer
    log(V5_DEBG, "%s : merge %i buffers, max. size %i\n", 
                _job->toStr(), _clause_buffers.size(), totalSize);
    std::vector<std::vector<int>*> buffers;
    for (auto& buf : _clause_buffers) buffers.push_back(&buf);
    std::vector<int> vec = merge(buffers, totalSize);
    testConsistency(vec, totalSize);

    // Reset clause buffers
    _clause_buffers.clear();
    return vec;
}

std::vector<int> AnytimeSatClauseCommunicator::merge(const std::vector<std::vector<int>*>& buffers, size_t maxSize) {
    std::vector<int> result;

    // Position counter for each buffer
    std::vector<int> positions(buffers.size(), 0);

    // How many VIP clauses in each buffer?
    std::vector<int> nvips(buffers.size());
    int totalNumVips = 0;
    for (size_t i = 0; i < buffers.size(); i++) {
        nvips[i] = (buffers[i]->size() > 0) ? buffers[i]->at(positions[i]) : 0;
        totalNumVips += nvips[i];
        positions[i]++;
    } 

    // Store number of VIP clauses of resulting buffer here
    result.push_back(0);
    int& resvips = result[0];

    std::vector<int> cls;
    int picked = -1;
    while (totalNumVips > 0) {
        do picked = (picked+1) % nvips.size(); while (nvips[picked] == 0);
        int& pos = positions[picked];
        int lit = buffers[picked]->at(pos++);
        // Append to clause
        cls.push_back(lit);
        if (lit == 0) {
            // Clause finished

            // Clause buffer size limit reached?
            if (result.size() + cls.size() > maxSize) break;

            // Clause not seen yet?
            if (_clause_filter.insert(cls).second) {
                // Insert clause into result clause buffer
                result.insert(result.end(), cls.begin(), cls.end());
                resvips++;
            }

            /*
            Logger::append(V5_DEBG, "VIP ");
            for (int l : cls) Logger::append(V5_DEBG, "%i ", l);
            log(V5_DEBG, "\n");*/

            // Clear clause vector, update counters
            cls.clear();
            nvips[picked]--;
            totalNumVips--;
        }
    }

    int clauseLength = 1;
    bool doContinue = true;
    while (doContinue) {
        doContinue = false;

        if (result.size() + 1 + clauseLength > maxSize) {
            // No clauses of this size are fitting into the buffer any more: stop
            break;
        }

        // Get number of clauses of clauseLength for each buffer
        // and also the sum over all these numbers
        std::vector<int> nclsoflen(buffers.size());
        int allclsoflen = 0;
        for (size_t i = 0; i < buffers.size(); i++) {
            nclsoflen[i] = positions[i] < (int)buffers[i]->size() ? 
                            buffers[i]->at(positions[i]) : 0;
            if (positions[i] < (int)buffers[i]->size()) doContinue = true;
            allclsoflen += nclsoflen[i];
            positions[i]++;
        }

        // Store number of inserted clauses of clauseLength in result[numpos]
        result.push_back(0);
        int numpos = result.size()-1;
        
        // Read clauses from buffers in a cyclic manner
        int picked = -1;
        while (allclsoflen > 0) {
            // Limit reached?
            if (result.size() + clauseLength > maxSize) {
                doContinue = false;
                break;
            }

            // Identify next clause
            do picked = (picked+1) % nvips.size(); while (nclsoflen[picked] == 0);
            const std::vector<int>& vec = *buffers[picked];
            int pos = positions[picked];
            auto begin = vec.begin()+pos;
            auto end = vec.begin()+pos+clauseLength;

            // Clause not included yet?
            if (_clause_filter.insert(std::vector<int>(begin, end)).second) {
                // Insert and increase corresponding counters
                result.insert(result.end(), begin, end);
                result[numpos]++;
            }

            /*
            Logger::append(V5_DEBG, "CLS ");
            for (int i = pos; i < pos+clauseLength; i++) 
                Logger::append(V5_DEBG, "%i ", vec[i]);
            log(V5_DEBG, "\n");*/

            // Update counters for remaining clauses 
            positions[picked] += clauseLength;
            nclsoflen[picked]--;
            allclsoflen--;
        }

        clauseLength++;
    }

    // Remove trailing zeroes because they are unnecessary
    // (as long as the buffer does not become empty)
    while (result.size() > 1 && result.back() == 0 && result[result.size()-2] == 0) 
        result.pop_back();

    _clause_filter.clear();
    return result;
}

bool AnytimeSatClauseCommunicator::testConsistency(std::vector<int>& buffer, size_t maxSize) {
    if (buffer.empty()) return true;

    if (maxSize > 0 && buffer.size() > maxSize) {
        log(V0_CRIT, "Clause buffer too full (%i/%i) - aborting\n", buffer.size(), maxSize);
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
        for (int offset = 1; offset <= numCls * length; offset++) {
            if (pos+offset >= buffer.size()) {
                consistent = 5; break;
            }
            int lit = buffer[pos+offset];
            if (lit == 0) {
                consistent = 6; break;
            }
        }
        pos += numCls * length + 1;
        length++;
    }

    if (consistent > 0) {
        log(V0_CRIT, "Consistency ERROR %i in clause buffer at position %i: \n", consistent, pos);
        for (size_t p = 0; p < buffer.size(); p++) {
            if (p == pos) log(LOG_NO_PREFIX | V0_CRIT, "(%i) ", buffer[p]);
            else          log(LOG_NO_PREFIX | V0_CRIT, "%i ", buffer[p]);
        }
        log(LOG_NO_PREFIX | V0_CRIT, "\n");
        abort();
    }
    return consistent == 0;
}