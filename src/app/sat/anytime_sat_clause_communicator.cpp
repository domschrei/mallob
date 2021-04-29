
#include "anytime_sat_clause_communicator.hpp"

#include "util/logger.hpp"
#include "comm/mympi.hpp"
#include "hordesat/utilities/clause_filter.hpp"

void AnytimeSatClauseCommunicator::handle(int source, JobMessage& msg) {

    if (msg.tag == MSG_GATHER_CLAUSES) {
        // Gather received clauses, send to parent

        int numAggregated = msg.payload.back();
        msg.payload.pop_back();
        std::vector<int>& clauses = msg.payload;
        testConsistency(clauses, getBufferLimit(numAggregated, BufferMode::ALL), /*sortByLbd=*/false);
        
        log(V5_DEBG, "%s : receive s=%i\n", _job->toStr(), clauses.size());
        
        // Add received clauses to local set of collected clauses
        _clause_buffers.push_back(clauses);
        _num_aggregated_nodes += numAggregated;

        if (canSendClauses()) {
            if (_job->getJobTree().isRoot()) {
                // Rank zero: log the broadcast
                log(V2_INFO, "%s : Distribute clause buffer from %i sources\n", _job->toStr(), _num_aggregated_nodes+1);
            }
            sendClausesToParent();
        }

    } else if (msg.tag == MSG_DISTRIBUTE_CLAUSES) {
        // Learn received clauses, send them to children
        broadcastAndLearn(msg.payload);
    }
}

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
        broadcastAndLearn(clausesToShare);
    } else {
        // Send set of clauses to parent
        int parentRank = _job->getJobTree().getParentNodeRank();
        JobMessage msg;
        msg.jobId = _job->getId();
        msg.epoch = 0; // unused
        msg.tag = MSG_GATHER_CLAUSES;
        msg.payload = clausesToShare;
        log(LOG_ADD_DESTRANK | V4_VVER, "%s : gather s=%i", parentRank, _job->toStr(), msg.payload.size());
        msg.payload.push_back(_num_aggregated_nodes);
        MyMpi::isend(MPI_COMM_WORLD, parentRank, MSG_SEND_APPLICATION_MESSAGE, msg);
    }

    _num_aggregated_nodes = 0;
}

void AnytimeSatClauseCommunicator::broadcastAndLearn(std::vector<int>& clauses) {
    testConsistency(clauses, 0, /*sortByLbd=*/false);
    sendClausesToChildren(clauses);
    learnClauses(clauses);
}

void AnytimeSatClauseCommunicator::learnClauses(std::vector<int>& clauses) {
    log(V4_VVER, "%s : learn s=%i\n", _job->toStr(), clauses.size());
    
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

void AnytimeSatClauseCommunicator::sendClausesToChildren(std::vector<int>& clauses) {
    
    // Send clauses to children
    JobMessage msg;
    msg.jobId = _job->getId();
    msg.epoch = 0; // unused
    msg.tag = MSG_DISTRIBUTE_CLAUSES;
    msg.payload = clauses;
    int childRank;
    if (_job->getJobTree().hasLeftChild()) {
        childRank = _job->getJobTree().getLeftChildNodeRank();
        log(LOG_ADD_DESTRANK | V4_VVER, "%s : broadcast s=%i", childRank, _job->toStr(), msg.payload.size());
        MyMpi::isend(MPI_COMM_WORLD, childRank, MSG_SEND_APPLICATION_MESSAGE, msg);
    }
    if (_job->getJobTree().hasRightChild()) {
        childRank = _job->getJobTree().getRightChildNodeRank();
        log(LOG_ADD_DESTRANK | V4_VVER, "%s : broadcast s=%i", childRank, _job->toStr(), msg.payload.size());
        MyMpi::isend(MPI_COMM_WORLD, childRank, MSG_SEND_APPLICATION_MESSAGE, msg);
    }
}

std::vector<int> AnytimeSatClauseCommunicator::prepareClauses() {

    // +1 for local clauses, but at most as many contributions as there are nodes
    _num_aggregated_nodes = std::min(_num_aggregated_nodes+1, _job->getJobTree().getCommSize());

    assert(_num_aggregated_nodes > 0);
    int totalSize = getBufferLimit(_num_aggregated_nodes, BufferMode::ALL);
    int selfSize = getBufferLimit(_num_aggregated_nodes, BufferMode::SELF);
    log(V5_DEBG, "%s : aggr=%i max_self=%i max_total=%i\n", _job->toStr(), 
            _num_aggregated_nodes, selfSize, totalSize);

    // Locally collect clauses from own solvers, add to clause buffer
    std::vector<int> selfClauses;
    // If not fully initialized yet, broadcast an empty set of clauses
    if (_job->getState() != ACTIVE || !_job->isInitialized() || !_job->hasPreparedSharing()) {
        selfClauses = std::vector<int>();
    } else {
        // Else, retrieve clauses from solvers
        log(V4_VVER, "%s : collect s<=%i\n", 
                    _job->toStr(), selfSize);
        selfClauses = _job->getPreparedClauses();
        testConsistency(selfClauses, 0 /*do not check buffer's size limit*/, /*sortByLbd=*/_sort_by_lbd);
    }
    _clause_buffers.push_back(std::move(selfClauses));

    // Merge all collected buffer into a single buffer
    log(V5_DEBG, "%s : merge n=%i s<=%i\n", 
                _job->toStr(), _clause_buffers.size(), totalSize);
    std::vector<int> vec = merge(totalSize);
    testConsistency(vec, totalSize, /*sortByLbd=*/false);

    // Reset clause buffers
    _clause_buffers.clear();
    return vec;
}

std::vector<int> AnytimeSatClauseCommunicator::merge(size_t maxSize) {
    std::vector<int> result;

    // Position counter for each buffer
    std::vector<int> positions(_clause_buffers.size(), 0);

    // How many VIP clauses in each buffer?
    std::vector<int> nvips(_clause_buffers.size());
    int totalNumVips = 0;
    for (size_t i = 0; i < _clause_buffers.size(); i++) {
        nvips[i] = (_clause_buffers[i].size() > 0) ? _clause_buffers[i][positions[i]] : 0;
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
        int lit = _clause_buffers[picked][pos++];
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
        std::vector<int> nclsoflen(_clause_buffers.size());
        int allclsoflen = 0;
        for (size_t i = 0; i < _clause_buffers.size(); i++) {
            nclsoflen[i] = positions[i] < (int)_clause_buffers[i].size() ? 
                            _clause_buffers[i][positions[i]] : 0;
            if (positions[i] < (int)_clause_buffers[i].size()) doContinue = true;
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
            if (clauseLength > 1 && _sort_by_lbd) {
                size_t lowestLbd = maxSize;
                for (size_t x = 0; x < nclsoflen.size(); x++) {
                    size_t i = (picked+1+x) % nclsoflen.size();
                    if (nclsoflen[i] > 0 && _clause_buffers[i][positions[i]] < lowestLbd) {
                        picked = i;
                        lowestLbd = _clause_buffers[i][positions[i]];
                    }
                }
                //log(V4_VVER, "pos=%i len=%i lbd=%i\n", result.size(), clauseLength, lowestLbd);
            } else {
                do picked = (picked+1) % nclsoflen.size(); while (nclsoflen[picked] == 0);
            }
            const std::vector<int>& vec = _clause_buffers[picked];
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

bool AnytimeSatClauseCommunicator::testConsistency(std::vector<int>& buffer, size_t maxSize, bool sortByLbd) {
    if (buffer.empty()) return true;

    if (maxSize > 0 && buffer.size() > maxSize) {
        log(V0_CRIT, "Clause buffer too full (%i/%i) - aborting\n", buffer.size(), maxSize);
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
                    || log_return_false("%i %i %i\n", length, currentLbd, originalClauses[clsIndex*length]));
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
        log(V0_CRIT, "Consistency ERROR %i in clause buffer at position %i: \n", consistent, pos);
        for (size_t p = 0; p < buffer.size(); p++) {
            if (p == pos) log(LOG_NO_PREFIX | V0_CRIT, "(%i) ", buffer[p]);
            else          log(LOG_NO_PREFIX | V0_CRIT, "%i ", buffer[p]);
        }
        log(LOG_NO_PREFIX | V0_CRIT, "\n");
        Logger::getMainInstance().flush();
        abort();
    }
    return consistent == 0;
}