
#include "anytime_sat_clause_communicator.hpp"

#include "util/console.hpp"
#include "comm/mympi.hpp"
#include "hordesat/utilities/clause_filter.hpp"

float AnytimeSatClauseCommunicator::getBufferLimit(int numAggregatedNodes) {
    return _clause_buf_base_size * std::pow(_clause_buf_discount_factor, std::log2(numAggregatedNodes+1));
}

bool AnytimeSatClauseCommunicator::canSendClauses() {
    if (!_initialized) return false;

    int numChildren = 0;
    // Must have received clauses from each existing children
    if (_job->hasLeftChild()) numChildren++;
    if (_job->hasRightChild()) numChildren++;

    if (_clause_buffers.size() >= numChildren) {
        if (!_job->hasPreparedSharing()) {
            int limit = std::ceil(getBufferLimit(_num_aggregated_nodes+1) / (_num_aggregated_nodes+1));
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

    if (_job->isRoot()) {
        // Share complete set of clauses to children
        Console::log(Console::VVERB, "%s : switch gather => broadcast", _job->toStr()); 
        learnClauses(clausesToShare);
        sendClausesToChildren(clausesToShare);
    } else {
        // Send set of clauses to parent
        int parentRank = _job->getParentNodeRank();
        JobMessage msg;
        msg.jobId = _job->getId();
        msg.epoch = 0; // unused
        msg.tag = MSG_GATHER_CLAUSES;
        msg.payload = clausesToShare;
        msg.payload.push_back(_num_aggregated_nodes);
        Console::log_send(Console::VERB, parentRank, "%s : gather", _job->toStr());
        MyMpi::isend(MPI_COMM_WORLD, parentRank, MSG_JOB_COMMUNICATION, msg);
    }

    _num_aggregated_nodes = 0;
}

void AnytimeSatClauseCommunicator::handle(int source, JobMessage& msg) {
    if (!_initialized || _job->isNotInState({JobState::ACTIVE}))
        return;

    if (msg.tag == MSG_GATHER_CLAUSES) {
        // Gather received clauses, send to parent
        // TODO count each child only once
        int numAggregated = msg.payload.back();
        msg.payload.pop_back();
        std::vector<int>& clauses = msg.payload;
        testConsistency(clauses);
        
        Console::log(Console::VVVERB, "%s : receive, size %i", _job->toStr(), clauses.size());
        
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

void AnytimeSatClauseCommunicator::learnClauses(const std::vector<int>& clauses) {
    Console::log(Console::VERB, "%s : learn, size %i", _job->toStr(), clauses.size());
    testConsistency(clauses);
    
    if (clauses.size() > 0) {
        // Locally learn clauses
        
        // If not active or not fully initialized yet: discard clauses
        if (_job->isNotInState({ACTIVE}) || !_job->isInitialized()) {
            Console::log(Console::VVERB, "%s : discard buffer, job is not (yet?) active", 
                    _job->toStr());
            return;
        }

        // Locally digest clauses
        Console::log(Console::VVERB, "%s : digest", _job->toStr());
        _job->digestSharing(clauses);
        Console::log(Console::VERB, "%s : digested", _job->toStr());
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
    if (_job->hasLeftChild()) {
        childRank = _job->getLeftChildNodeRank();
        Console::log_send(Console::VVERB, childRank, "%s : broadcast", _job->toStr());
        MyMpi::isend(MPI_COMM_WORLD, childRank, MSG_JOB_COMMUNICATION, msg);
    }
    if (_job->hasRightChild()) {
        childRank = _job->getRightChildNodeRank();
        Console::log_send(Console::VVERB, childRank, "%s : broadcast", _job->toStr());
        MyMpi::isend(MPI_COMM_WORLD, childRank, MSG_JOB_COMMUNICATION, msg);
    }
}

std::vector<int> AnytimeSatClauseCommunicator::prepareClauses() {

    // +1 for local clauses
    _num_aggregated_nodes++;

    assert(_num_aggregated_nodes > 0);
    float s = getBufferLimit(_num_aggregated_nodes);
    int totalSize = std::ceil(s);
    int selfSize = std::ceil(s / _num_aggregated_nodes);
    Console::log(Console::VVVERB, "%s : aggregated=%i max_self=%i max_total=%i", _job->toStr(), 
            _num_aggregated_nodes, selfSize, totalSize);

    // Locally collect clauses from own solvers, add to clause buffer
    std::vector<int> selfClauses;
    // If not fully initialized yet, broadcast an empty set of clauses
    if (_job->isNotInState({ACTIVE}) || !_job->isInitialized() || !_job->hasPreparedSharing()) {
        selfClauses = std::vector<int>();
    } else {
        // Else, retrieve clauses from solvers
        Console::log(Console::VVERB, "%s : collect local cls, max. size %i", 
                    _job->toStr(), selfSize);
        selfClauses = _job->getPreparedClauses();
        testConsistency(selfClauses);
    }
    _clause_buffers.push_back(selfClauses);

    // Merge all collected buffer into a single buffer
    Console::log(Console::VVVERB, "%s : merge %i buffers, max. size %i", 
                _job->toStr(), _clause_buffers.size(), totalSize);
    std::vector<std::vector<int>*> buffers;
    for (auto& buf : _clause_buffers) buffers.push_back(&buf);
    std::vector<int> vec = merge(buffers, totalSize);
    testConsistency(vec);

    // Reset clause buffers
    _clause_buffers.clear();
    return vec;
}

std::vector<int> AnytimeSatClauseCommunicator::merge(const std::vector<std::vector<int>*>& buffers, int maxSize) {
    std::vector<int> result;

    // Position counter for each buffer
    std::vector<int> positions(buffers.size(), 0);

    // How many VIP clauses in each buffer?
    std::vector<int> nvips(buffers.size());
    int totalNumVips = 0;
    for (int i = 0; i < buffers.size(); i++) {
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
            if (result.size() + cls.size() > maxSize)
                return result;

            // Clause not seen yet?
            if (_clause_filter.insert(cls).second) {
                // Insert clause into result clause buffer
                result.insert(result.end(), cls.begin(), cls.end());
                resvips++;
            }

            /*
            Console::append(Console::VVVERB, "VIP ");
            for (int l : cls) Console::append(Console::VVVERB, "%i ", l);
            Console::log(Console::VVVERB, "");*/

            // Clear clause vector, update counters
            cls.clear();
            nvips[picked]--;
            totalNumVips--;
        }
    }

    int clauseLength = 1;
    bool anyLeft = true;
    while (anyLeft) {
        anyLeft = false;

        // Get number of clauses of clauseLength for each buffer
        // and also the sum over all these numbers
        std::vector<int> nclsoflen(buffers.size());
        int allclsoflen = 0;
        for (int i = 0; i < buffers.size(); i++) {
            nclsoflen[i] = positions[i] < buffers[i]->size() ? 
                            buffers[i]->at(positions[i]) : 0;
            if (positions[i] < buffers[i]->size()) anyLeft = true;
            allclsoflen += nclsoflen[i];
            positions[i]++;
        }

        // Store number of inserted clauses of clauseLength in result[numpos]
        result.push_back(0);
        int numpos = result.size()-1;
        
        // Clear filter to only consider clauses of upcoming length
        // Actually, don't. Takes too much work.
        //_clause_filter.clear();

        // Read clauses from buffers in a cyclic manner
        int picked = -1;
        while (allclsoflen > 0) {
            // Limit reached?
            if (result.size() + clauseLength > maxSize) return result;

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
            Console::append(Console::VVVERB, "CLS ");
            for (int i = pos; i < pos+clauseLength; i++) 
                Console::append(Console::VVVERB, "%i ", vec[i]);
            Console::log(Console::VVVERB, "");*/

            // Update counters for remaining clauses 
            positions[picked] += clauseLength;
            nclsoflen[picked]--;
            allclsoflen--;
        }

        clauseLength++;
    }

    _clause_filter.clear();
    return result;
}

bool AnytimeSatClauseCommunicator::testConsistency(const std::vector<int>& buffer) {
    if (buffer.empty()) return true;

    int consistent = 0;
    int pos = 0;

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
        Console::log(Console::CRIT, "Consistency ERROR %i in clause buffer at position %i", consistent, pos);
        for (int p = 0; p < buffer.size(); p++) {
            if (p == pos) Console::append(Console::CRIT, "(%i) ", buffer[p]);
            else          Console::append(Console::CRIT, "%i ", buffer[p]);
        }
        Console::append(Console::CRIT, "\n");
        abort();
    }
    return consistent == 0;
}