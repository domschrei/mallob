#include "failed_assumption_communicator.hpp"

#include <algorithm>
#include <cassert>
#include <iterator>
#include <sstream>

#include "comm/mympi.hpp"

FailedAssumptionCommunicator::FailedAssumptionCommunicator(DynamicCubeSatJob &job, LoggingInterface &logger) : _job(job), _logger(logger) {}

void FailedAssumptionCommunicator::gather() {
    // May be called during ACTIVE or INITIALIZING_TO_ACTIVE
    // If the second one is the case, the methods in the DynamicCubeSatJob guarantees that the uninitialized lib is not accessed and dummy values are returned
    assert(_job.isActive());

    _logger.log(0, "FailedAssumptionCommunicator: Started gather");

    // Reset message counter
    _messageCounter = 0;

    // Get local failed assumptions and insert into received failed assumptions
    auto failed_assumption = _job.getFailedAssumptions();
    _received_failed_assumptions.insert(_received_failed_assumptions.end(), failed_assumption.begin(), failed_assumption.end());

    if (_job.isRoot()) {
        _logger.log(0, "FailedAssumptionCommunicator: This job is root -> 1. Persist everything received 2. Distribute all known failed assumptions");

        // TODO Do we have the problem here that a clause may be never added to the root filter because there was a random collision due to the bloom filter?
        persist(_received_failed_assumptions);

        _received_failed_assumptions.clear();

        // For the beginning we always distribute all found failed assumptions
        // Merge all clauses in the filter into one vector
        std::vector<int> clauses_to_distribute;
        for (auto &clause : _clause_filter) {
            clauses_to_distribute.insert(clauses_to_distribute.end(), clause.begin(), clause.end());
        }
        // TODO Improve this

        // If there are failed assumptions, distribute them
        if (!clauses_to_distribute.empty()) {
            distribute(clauses_to_distribute);
        }

    } else {
        _logger.log(0, "FailedAssumptionCommunicator: This job is not root -> send everything received to the parent");

        // Send everything in the accumulator to the parent
        JobMessage msg;
        msg.jobId = _job.getId();
        msg.epoch = 0;  // unused
        msg.tag = MSG_FAILED_ASSUMPTION_GATHER;
        msg.payload = _received_failed_assumptions;

        // Clear received failed assumptions
        _received_failed_assumptions.clear();

        int parentRank = _job.getParentNodeRank();
        log_send(parentRank, msg.payload, "gather");
        MyMpi::isend(MPI_COMM_WORLD, parentRank, MSG_SEND_APPLICATION_MESSAGE, msg);
    }
}

void FailedAssumptionCommunicator::persist(std::vector<int> &failed_assumptions) {
    // Failed assumption may be empty

    std::vector<int> new_clauses;
    std::vector<int> clause;

    for (auto lit : failed_assumptions) {
        if (lit == 0) {
            // No empty clauses allowed
            assert(!clause.empty());

            // Add delimiter at the end of clause
            clause.push_back(0);

            // Try to add the clause to the filter
            bool clause_is_new = _clause_filter.insert(clause).second;

            // If the clause is new add it to new clauses
            if (clause_is_new) new_clauses.insert(new_clauses.end(), clause.begin(), clause.end());

            // Clear local variable
            clause.clear();

        } else {
            clause.push_back(lit);
        }
    }

    // The failed assumptions should only contain valid clauses that are terminated by a zero -> clause should be empty here
    assert(clause.empty());

    // Pass new failed assumption to the lib
    if (!new_clauses.empty()) _job.digestFailedAssumptions(new_clauses);
}

void FailedAssumptionCommunicator::distribute(std::vector<int> &failed_assumptions) {
    JobMessage msg;
    msg.jobId = _job.getId();
    msg.epoch = 0;  // unused
    msg.tag = MSG_FAILED_ASSUMPTION_DISTRIBUTE;
    msg.payload = failed_assumptions;

    if (_job.hasLeftChild()) {
        int leftChildRank = _job.getLeftChildNodeRank();
        log_send(leftChildRank, msg.payload, "distribute");
        MyMpi::isend(MPI_COMM_WORLD, leftChildRank, MSG_SEND_APPLICATION_MESSAGE, msg);
    }

    if (_job.hasRightChild()) {
        int rightChildRank = _job.getRightChildNodeRank();
        log_send(rightChildRank, msg.payload, "distribute");
        MyMpi::isend(MPI_COMM_WORLD, rightChildRank, MSG_SEND_APPLICATION_MESSAGE, msg);
    }
}

void FailedAssumptionCommunicator::handle(int source, JobMessage &msg) {
    // Caller guarantees that the job cannot be interrupted

    // Can only be suspended or active, because the message was once in the job tree
    assert(_job.isSuspended() || _job.isActive());

    if (_job.isSuspended()) {
        // The call to suspend must already be finished
        assert(_received_failed_assumptions.empty());

        // The root node cannot be suspended
        assert(!_job.isRoot());

        // The possible message types in this situation
        assert(msg.tag == MSG_FAILED_ASSUMPTION_GATHER || msg.tag == MSG_FAILED_ASSUMPTION_DISTRIBUTE);

        // This message must be a remainder of before this job was suspended

        if (msg.tag == MSG_FAILED_ASSUMPTION_GATHER && !msg.payload.empty()) {
            _logger.log(0, "FailedAssumptionCommunicator: Received not empty gather while suspended -> send failed assumptions to root");

            // Send the failed assumptions that would be lost to the root node
            sendToRoot(msg.payload);

        } else if (msg.tag == MSG_FAILED_ASSUMPTION_DISTRIBUTE) {
            assert(!msg.payload.empty());

            _logger.log(0, "FailedAssumptionCommunicator: Received distribute while suspended -> ignore");
        }

        // Even when the job is suspended, new failed cubes may be learnt
        // if (msg.tag == MSG_FAILED_ASSUMPTION_DISTRIBUTE && _job.appl_doneInitializing()) {
        //     persist(msg.payload);
        // }
        // Currently MSG_FAILED_ASSUMPTION_DISTRIBUTE is ignored

    } else {
        // The job is active
        if (msg.tag == MSG_FAILED_ASSUMPTION_GATHER) {
            _logger.log(0, "FailedAssumptionCommunicator: Received gather while active -> 1. Add received 2. Increment message counter 3. Maybe gather");

            // The payload may be empty
            if (!msg.payload.empty()) {
                // Insert received failed assumptions
                _received_failed_assumptions.insert(_received_failed_assumptions.end(), msg.payload.begin(), msg.payload.end());
            }

            // Count children
            int numChildren = 0;
            if (_job.hasLeftChild()) numChildren++;
            if (_job.hasRightChild()) numChildren++;

            if (++_messageCounter >= numChildren) {
                gather();
            }

        } else if (msg.tag == MSG_FAILED_ASSUMPTION_DISTRIBUTE) {
            // The payload may not be empty
            assert(!msg.payload.empty());

            _logger.log(0, "FailedAssumptionCommunicator: Received distribute while active -> 1. If initialized persist 2. Distribute to children");

            // If the lib is initialized, persist the distributed failed assumptions
            if (_job.appl_doneInitializing()) persist(msg.payload);

            distribute(msg.payload);

        } else if (msg.tag == MSG_FAILED_ASSUMPTION_SEND_TO_ROOT) {
            // The payload may not be empty
            assert(!msg.payload.empty());

            // This message may only be send to the root node
            assert(_job.isRoot());

            _logger.log(0, "FailedAssumptionCommunicator: Received send to root -> Add received");

            // Insert received failed assumptions
            _received_failed_assumptions.insert(_received_failed_assumptions.end(), msg.payload.begin(), msg.payload.end());
        }
    }
}

void FailedAssumptionCommunicator::sendToRoot(std::vector<int> &failed_assumptions) {
    JobMessage msg;
    msg.jobId = _job.getId();
    msg.epoch = 0;  // unused
    msg.tag = MSG_FAILED_ASSUMPTION_SEND_TO_ROOT;
    msg.payload = failed_assumptions;

    int rootNodeRank = _job.getRootNodeRank();
    log_send(rootNodeRank, msg.payload, "sendToRoot");
    MyMpi::isend(MPI_COMM_WORLD, rootNodeRank, MSG_SEND_APPLICATION_MESSAGE, msg);
}

bool FailedAssumptionCommunicator::isFailedAssumptionMessage(int tag) {
    return tag == MSG_FAILED_ASSUMPTION_DISTRIBUTE || tag == MSG_FAILED_ASSUMPTION_GATHER || tag == MSG_FAILED_ASSUMPTION_SEND_TO_ROOT;
}

void FailedAssumptionCommunicator::log_send(int destRank, std::vector<int> &payload, const char *str, ...) {
    // Convert payload
    // auto payloadString = payloadToString(payload);

    // For addtitional printf params
    va_list vl;
    // Puts first value in parameter list into str
    va_start(vl, str);
    std::string output = std::string(str) + " => [" + std::to_string(destRank) + "] { #" + std::to_string(payload.size()) + "}";
    _logger.log_va_list(0, output.c_str(), vl);
    va_end(vl);
}

std::string FailedAssumptionCommunicator::payloadToString(std::vector<int> &payload) {
    // https://www.DynamicCubeCommunicatorgeeksforgeeks.org/transform-vector-string/
    if (!payload.empty()) {
        std::ostringstream stringStream;
        // Convert all but the last element to avoid a trailing ","
        std::copy(payload.begin(), payload.end() - 1, std::ostream_iterator<int>(stringStream, ", "));

        // Now add the last element with no delimiter
        stringStream << payload.back();

        return stringStream.str();

    } else {
        return "";
    }
}