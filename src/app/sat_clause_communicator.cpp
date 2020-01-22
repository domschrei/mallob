
#include "sat_clause_communicator.h"

#include "util/console.h"
#include "util/mympi.h"

void SatClauseCommunicator::initiateCommunication() {

    JobMessage msg;
    if (_job->isRoot()) {
        // There are no other nodes computing on this job:
        // internally learn collected clauses, if ACTIVE
        int jobCommEpoch = _job->getJobCommEpoch();
        if (_job->isInState({ACTIVE})) {
            msg.payload = collectClausesFromSolvers();
            learnClausesFromAbove(msg.payload, jobCommEpoch);
        }
        _last_shared_job_comm = jobCommEpoch;
        return;
    }
    msg.jobId = _job->getId();
    msg.epoch = _job->getJobCommEpoch();
    msg.tag = MSG_GATHER_CLAUSES;
    msg.payload = collectClausesFromSolvers();
    int parentRank = _job->getParentNodeRank();
    Console::log_send(Console::VERB, parentRank, "%s : (JCE=%i) sending, size %i", _job->toStr(), msg.epoch, msg.payload.size());
    MyMpi::isend(MPI_COMM_WORLD, parentRank, MSG_JOB_COMMUNICATION, msg);
    // TODO //stats.increase("sentMessages");
}

void SatClauseCommunicator::continueCommunication(int source, JobMessage& msg) {

    if (_job->isNotInState({JobState::ACTIVE}))
        return;

    // Unpack job message
    int jobId = msg.jobId;
    int epoch = msg.epoch;
    std::vector<int>& clauses = msg.payload;

    if (msg.tag == MSG_GATHER_CLAUSES) {
        // Gather received clauses, send to parent
        
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

            std::vector<int> clausesToShare = shareCollectedClauses(epoch);
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
                Console::log_send(Console::VERB, parentRank, "%s : (JCE=%i) gathering", _job->toStr(), epoch);
                MyMpi::isend(MPI_COMM_WORLD, parentRank, MSG_JOB_COMMUNICATION, msg);
            }
            _last_shared_job_comm = epoch;
        }

    } else if (msg.tag == MSG_DISTRIBUTE_CLAUSES) {
        // Learn received clauses, send them to children
        learnAndDistributeClausesDownwards(clauses, epoch);
    }
}

void SatClauseCommunicator::learnAndDistributeClausesDownwards(std::vector<int>& clauses, int jobCommEpoch) {

    Console::log(Console::VVERB, "%s : (JCE=%i) learning, size %i", _job->toStr(), jobCommEpoch, clauses.size());
    assert(clauses.size() % BROADCAST_CLAUSE_INTS_PER_NODE == 0);

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

std::vector<int> SatClauseCommunicator::collectClausesFromSolvers() {

    // If not fully initialized yet, broadcast an empty set of clauses
    if (_job->isNotInState({ACTIVE}) || !_job->getSolver()->isFullyInitialized()) {
        return std::vector<int>(BROADCAST_CLAUSE_INTS_PER_NODE, 0);
    }
    // Else, retrieve clauses from solvers
    return _job->getSolver()->prepareSharing();
}
void SatClauseCommunicator::insertIntoClauseBuffer(std::vector<int>& vec, int jobCommEpoch) {

    // If there are clauses in the buffer which are from a previous job comm epoch:
    if (!_clause_buffer.empty() && _job_comm_epoch_of_clause_buffer != jobCommEpoch) {
        // Previous clauses came from an old epoch; reset clause buffer
        Console::log(Console::VVERB, "(JCE=%i) Discarding buffer, size %i, from old JCE %i", 
                jobCommEpoch, _clause_buffer.size(), _job_comm_epoch_of_clause_buffer);
        _num_clause_sources = 0;
        _clause_buffer.resize(0);
    }
    // Update epoch of current clause buffer
    _job_comm_epoch_of_clause_buffer = jobCommEpoch;

    // Insert clauses into local clause buffer for later sharing
    _clause_buffer.insert(_clause_buffer.end(), vec.begin(), vec.end());

    // Resize to multiple of #clause-ints per node
    int prevSize = _clause_buffer.size();
    int remainder = prevSize % BROADCAST_CLAUSE_INTS_PER_NODE;
    if (remainder != 0) {
        _clause_buffer.resize(prevSize + BROADCAST_CLAUSE_INTS_PER_NODE-remainder);
        std::fill(_clause_buffer.begin()+prevSize, _clause_buffer.end(), 0);
    }
    assert(_clause_buffer.size() % BROADCAST_CLAUSE_INTS_PER_NODE == 0);
}
void SatClauseCommunicator::collectClausesFromBelow(std::vector<int>& clauses, int jobCommEpoch) {

    insertIntoClauseBuffer(clauses, jobCommEpoch);
    _num_clause_sources++;
}
bool SatClauseCommunicator::canShareCollectedClauses() {

    int numChildren = 0;
    // Must have received clauses from both children,
    // except if one / both of them cannot exist according to volume
    if (_job->hasLeftChild()) numChildren++;
    if (_job->hasRightChild()) numChildren++;
    return numChildren == _num_clause_sources;
}
std::vector<int> SatClauseCommunicator::shareCollectedClauses(int jobCommEpoch) {

    // Locally collect clauses from own solvers, add to clause buffer
    std::vector<int> selfClauses = collectClausesFromSolvers();
    insertIntoClauseBuffer(selfClauses, jobCommEpoch);
    std::vector<int> vec = _clause_buffer;

    // Reset clause buffer
    _num_clause_sources = 0;
    _clause_buffer.resize(0);
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
    /*} else {
        Console::log(Console::VVERB, "%s : (JCE=%i) discarded because job is being manipulated", 
            toStr(), jobCommEpoch);
    }*/
}