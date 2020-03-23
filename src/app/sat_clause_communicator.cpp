
#include "sat_clause_communicator.h"

#include "util/console.h"
#include "util/mympi.h"

void SatClauseCommunicator::initiateCommunication() {
    if (!_initialized) return;

    JobMessage msg;
    if (_job->isRoot()) {
        // There are no other nodes computing on this job:
        // internally learn collected clauses, if ACTIVE
        int jobCommEpoch = _job->getJobCommEpoch();
        if (_job->isInState({ACTIVE})) {
            msg.payload = collectClausesFromSolvers(std::ceil(_clause_buf_discount_factor * _clause_buf_base_size), jobCommEpoch);
            learnClausesFromAbove(msg.payload, jobCommEpoch);
        }
        _last_shared_job_comm = jobCommEpoch;
        return;
    }
    msg.jobId = _job->getId();
    msg.epoch = _job->getJobCommEpoch();
    msg.tag = MSG_GATHER_CLAUSES;
    msg.payload = collectClausesFromSolvers(std::ceil(_clause_buf_discount_factor * _clause_buf_base_size), msg.epoch);
    testConsistency(msg.payload);
    msg.payload.push_back(0); // last int: depth the clause buffer traversed through the job tree so far.
    int parentRank = _job->getParentNodeRank();
    Console::log_send(Console::VERB, parentRank, "%s : (JCE=%i) sending, size %i", _job->toStr(), msg.epoch, msg.payload.size());
    MyMpi::isend(MPI_COMM_WORLD, parentRank, MSG_JOB_COMMUNICATION, msg);
    // TODO //stats.increase("sentMessages");
}

void SatClauseCommunicator::continueCommunication(int source, JobMessage& msg) {

    if (!_initialized || _job->isNotInState({JobState::ACTIVE}))
        return;

    // Unpack job message
    int jobId = msg.jobId;
    int epoch = msg.epoch;

    if (msg.tag == MSG_GATHER_CLAUSES) {
        // Gather received clauses, send to parent

        int passedLayers = msg.payload.back() + 1;
        msg.payload.pop_back();
        std::vector<int>& clauses = msg.payload;
        testConsistency(clauses);
        
        Console::log(Console::VERB, "%s : (JCE=%i) received, size %i", _job->toStr(), epoch, clauses.size());

        if (_last_shared_job_comm >= epoch) {
            // Already shared clauses upwards this job comm epoch!
            Console::log(Console::VERB, "%s : (JCE=%i) ending: already did sharing this JCE", _job->toStr(), epoch);
            Console::log(Console::VERB, "%s : (JCE=%i) learning and broadcasting down", _job->toStr(), epoch);
            learnAndDistributeClausesDownwards(clauses, epoch);
            return;
        }
        
        // Add received clauses to local set of collected clauses
        collectClausesFromBelow(clauses, epoch);

        // Ready to share the clauses?
        if (canShareCollectedClauses()) {

            std::vector<int> clausesToShare = shareCollectedClauses(epoch, passedLayers);
            if (_job->isRoot()) {
                // Share complete set of clauses to children
                Console::log(Console::VERB, "%s : (JCE=%i) switching: gather => broadcast", _job->toStr(), epoch); 
                learnAndDistributeClausesDownwards(clausesToShare, epoch);
            } else {
                // Send set of clauses to parent
                int parentRank = _job->getParentNodeRank();
                JobMessage msg;
                msg.jobId = jobId;
                msg.epoch = epoch;
                msg.tag = MSG_GATHER_CLAUSES;
                msg.payload = clausesToShare;
                msg.payload.push_back(passedLayers);
                Console::log_send(Console::VERB, parentRank, "%s : (JCE=%i) gathering", _job->toStr(), epoch);
                MyMpi::isend(MPI_COMM_WORLD, parentRank, MSG_JOB_COMMUNICATION, msg);
            }
            _last_shared_job_comm = epoch;
        }

    } else if (msg.tag == MSG_DISTRIBUTE_CLAUSES) {
        // Learn received clauses, send them to children

        std::vector<int>& clauses = msg.payload;
        testConsistency(clauses);

        learnAndDistributeClausesDownwards(clauses, epoch);
    }
}

void SatClauseCommunicator::learnAndDistributeClausesDownwards(std::vector<int>& clauses, int jobCommEpoch) {

    Console::log(Console::VVERB, "%s : (JCE=%i) learning, size %i", _job->toStr(), jobCommEpoch, clauses.size());

    // Send clauses to children
    JobMessage msg;
    msg.jobId = _job->getId();
    msg.epoch = jobCommEpoch;
    msg.tag = MSG_DISTRIBUTE_CLAUSES;
    msg.payload = clauses;
    int childRank;
    if (_job->hasLeftChild()) {
        childRank = _job->getLeftChildNodeRank();
        Console::log_send(Console::VERB, childRank, "%s : (JCE=%i) broadcasting", _job->toStr(), jobCommEpoch);
        MyMpi::isend(MPI_COMM_WORLD, childRank, MSG_JOB_COMMUNICATION, msg);
    }
    if (_job->hasRightChild()) {
        childRank = _job->getRightChildNodeRank();
        Console::log_send(Console::VERB, childRank, "%s : (JCE=%i) broadcasting", _job->toStr(), jobCommEpoch);
        MyMpi::isend(MPI_COMM_WORLD, childRank, MSG_JOB_COMMUNICATION, msg);
    }

    if (clauses.size() > 0) {
        // Locally learn clauses
        learnClausesFromAbove(clauses, jobCommEpoch);
    }
}

std::vector<int> SatClauseCommunicator::collectClausesFromSolvers(int maxSize, int jobCommEpoch) {

    // If not fully initialized yet, broadcast an empty set of clauses
    if (_job->isNotInState({ACTIVE}) || !_job->getSolver()->isFullyInitialized()) {
        return std::vector<int>();
    }
    // Else, retrieve clauses from solvers
    Console::log(Console::VVERB, "%s : (JCE=%i) Collecting local clauses, max. size %i", 
                _job->toStr(), jobCommEpoch, maxSize);
    return _job->getSolver()->prepareSharing(maxSize);
}
void SatClauseCommunicator::insertIntoClauseBuffer(std::vector<int>& vec, int jobCommEpoch) {

    // If there are clauses in the buffer which are from a previous job comm epoch:
    if (!_clause_buffers.empty() && _job_comm_epoch_of_clause_buffer != jobCommEpoch) {
        // Previous clauses came from an old epoch; reset clause buffer
        Console::log(Console::VVERB, "(JCE=%i) Discarding buffers from old JCE %i", 
                jobCommEpoch, _job_comm_epoch_of_clause_buffer);
        _num_clause_sources = 0;
        _clause_buffers.clear();
    }
    // Update epoch of current clause buffer
    //_job_comm_epoch_of_clause_buffer = std::max(_job_comm_epoch_of_clause_buffer, jobCommEpoch);
    _job_comm_epoch_of_clause_buffer = jobCommEpoch;

    // Insert clauses into local clause buffer for later sharing
    _clause_buffers.push_back(vec);

}
void SatClauseCommunicator::collectClausesFromBelow(std::vector<int>& clauses, int jobCommEpoch) {
    Console::log(Console::VVERB, "%s : (JCE=%i) local clause export", 
                _job->toStr(), jobCommEpoch);
    insertIntoClauseBuffer(clauses, jobCommEpoch);
    _num_clause_sources++;
}
bool SatClauseCommunicator::canShareCollectedClauses() {

    int numChildren = 0;
    // Must have received clauses from both children,
    // except if one / both of them cannot exist according to volume
    if (_job->hasLeftChild()) numChildren++;
    if (_job->hasRightChild()) numChildren++;
    return _num_clause_sources >= numChildren;
}
std::vector<int> SatClauseCommunicator::shareCollectedClauses(int jobCommEpoch, int passedLayers) {

    int selfSize = std::ceil(_clause_buf_base_size * std::pow(_clause_buf_discount_factor, passedLayers+1));
    int totalSize = selfSize;
    totalSize += _num_clause_sources * std::pow(_clause_buf_discount_factor, passedLayers+1) * (std::pow(2, passedLayers)-1) * _clause_buf_base_size;
    // std::pow(CLAUSE_EXCHANGE_MULTIPLIER, passedLayers);
    Console::log(Console::VVVERB, "traversed_layers=%i max_total_size=%i", passedLayers, totalSize);

    // Locally collect clauses from own solvers, add to clause buffer
    std::vector<int> selfClauses = collectClausesFromSolvers(selfSize, jobCommEpoch);
    testConsistency(selfClauses);
    insertIntoClauseBuffer(selfClauses, jobCommEpoch);

    // Merge all collected buffer into a single buffer
    Console::log(Console::VVERB, "%s : (JCE=%i) merging %i buffers into total size %i", 
                _job->toStr(), jobCommEpoch, _clause_buffers.size(), totalSize);
    std::vector<std::vector<int>*> buffers;
    for (auto& buf : _clause_buffers) buffers.push_back(&buf);
    std::vector<int> vec = merge(buffers, totalSize);
    testConsistency(vec);

    // Reset clause buffers
    _num_clause_sources = 0;
    _clause_buffers.clear();

    return vec;
}
void SatClauseCommunicator::learnClausesFromAbove(std::vector<int>& clauses, int jobCommEpoch) {

    // If not active or not fully initialized yet: discard clauses
    if (_job->isNotInState({ACTIVE}) || !_job->getSolver()->isFullyInitialized()) {
        Console::log(Console::VVERB, "%s : (JCE=%i) discarded because job is not (yet?) active", 
                _job->toStr(), jobCommEpoch);
        return;
    }

    // Locally digest clauses
    Console::log(Console::VVERB, "%s : (JCE=%i) digesting ...", _job->toStr(), jobCommEpoch);
    _job->lockHordeManipulation();
    if (_job->getSolver() != NULL) _job->getSolver()->digestSharing(clauses);
    _job->unlockHordeManipulation();
    Console::log(Console::VVERB, "%s : (JCE=%i) digested", _job->toStr(), jobCommEpoch);
}

std::vector<int> SatClauseCommunicator::merge(const std::vector<std::vector<int>*>& buffers, int maxSize) {
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

            // Insert clause into result clause buffer
            result.insert(result.end(), cls.begin(), cls.end());
            resvips++;

            Console::append(Console::VVVERB, "VIP ");
            for (int l : cls) Console::append(Console::VVVERB, "%i ", l);
            Console::log(Console::VVVERB, "");

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

        // Store number of inserted clauses of clauseLength in result
        result.push_back(0);
        int numpos = result.size()-1;
        
        // Read clauses from buffers in a cyclic manner
        int picked = -1;
        while (allclsoflen > 0) {
            // Limit reached?
            if (result.size() + clauseLength > maxSize) return result;

            do picked = (picked+1) % nvips.size(); while (nclsoflen[picked] == 0);
            const std::vector<int>& vec = *buffers[picked];
            int pos = positions[picked];

            /*
            Console::append(Console::VVVERB, "CLS ");
            for (int i = pos; i < pos+clauseLength; i++) 
                Console::append(Console::VVVERB, "%i ", vec[i]);
            Console::log(Console::VVVERB, "");*/

            result.insert(result.end(), vec.begin()+pos, vec.begin()+pos+clauseLength);
            positions[picked] += clauseLength;
            nclsoflen[picked]--;
            allclsoflen--;
            result[numpos]++;
        }

        clauseLength++;
    }

    return result;
}

bool SatClauseCommunicator::testConsistency(std::vector<int>& buffer) {
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
        Console::log(Console::CRIT, "Consistency error %i in clause buffer at position %i", consistent, pos);
        for (int p = 0; p < buffer.size(); p++) {
            if (p == pos) Console::append(Console::CRIT, "(%i) ", buffer[p]);
            else          Console::append(Console::CRIT, "%i ", buffer[p]);
        }
        Console::append(Console::CRIT, "\n");
        abort();
    }
    return consistent == 0;
}