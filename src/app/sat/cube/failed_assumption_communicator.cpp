#include "failed_assumption_communicator.hpp"

#include <algorithm>
#include <cassert>
#include <iterator>
#include <sstream>

#include "comm/mympi.hpp"

FailedAssumptionCommunicator::FailedAssumptionCommunicator(DynamicCubeSatJob &job, LoggingInterface &logger) : _job(job), _logger(logger) {}

// Returns an end iterator to the start of a clause in buffer
// with the range [start iterator, end iterator) containing the most possible clauses with a length less than the limit
//
// The parameter startIndex must be the index of a first literal of any clause in buffer
// The limit must be less than the std::distance(std::next(buffer.begin(), startIndex), buffer.end())
inline std::vector<int>::const_iterator findEndIteratorForLimit(const std::vector<int> &buffer, int startIndex, int limit) {
    assert(startIndex == 0 || buffer.at(startIndex - 1) == 0);

    // Create iterator to start index
    std::vector<int>::const_iterator start_iterator = std::next(buffer.begin(), startIndex);

    assert(*start_iterator == buffer.at(startIndex));

    // How many elements can be send before setting the start index back to the beginning
    int distance_to_end = std::distance(start_iterator, buffer.end());
    // Must be greater than or equal to two, since that is the smallest size for a clause
    assert(distance_to_end >= 2);

    // The limit must be less than the distance to the end otherwise the caller could just have gotten buffer.end()
    assert(limit < distance_to_end);

    std::vector<int>::const_reverse_iterator reverse_end_iterator = std::next(buffer.rbegin(), distance_to_end - limit);

    // The reverse iterator must be after or at most one element in front of the start iterator
    assert(reverse_end_iterator.base() - buffer.begin() >= startIndex);
    // The range [start_iterator, reverse_end_iterator.base()) should contain exactly #limit items
    assert(std::distance(start_iterator, reverse_end_iterator.base()) == limit);

    // Find closest zero or the start
    reverse_end_iterator = std::find(reverse_end_iterator, buffer.rend(), 0);

    assert(reverse_end_iterator == buffer.rend() || *reverse_end_iterator == 0);

    // Create iterator one element behind reverse_end_iterator
    // https://riptutorial.com/cplusplus/example/5101/reverse-iterators
    std::vector<int>::const_iterator end_iterator = reverse_end_iterator.base();

    // The distance between start iterator and end must be lower or equal to limit after find
    assert(std::distance(start_iterator, end_iterator) <= limit);

    return end_iterator;
}

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
        // Persist everything received
        persist(_received_failed_assumptions);

        // Clear received failed assumptions
        _received_failed_assumptions.clear();

        // Start distribute if there are new clauses or existing clauses
        if (!_new_clauses.empty() || !_all_clauses.empty()) {
            startDistribute();
        }
    } else {
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

    _new_clauses.clear();

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
            if (clause_is_new) _new_clauses.insert(_new_clauses.end(), clause.begin(), clause.end());

            // Clear local variable
            clause.clear();

        } else {
            clause.push_back(lit);
        }
    }

    // The failed assumptions should only contain valid clauses that are terminated by a zero -> clause should be empty here
    assert(clause.empty());

    // Pass new failed assumption to the lib
    if (!_new_clauses.empty()) _job.digestFailedAssumptions(_new_clauses);
}

void FailedAssumptionCommunicator::startDistribute() {
    // We set a fix limit for distribute
    // Every distribute we share the new clauses
    // Then we fill the buffer using old clauses, starting from the member _distribute_start_index
    const int limit = 20000;
    // TODO Rework using dynamic limit from AnyTimeSatClauseCommunicator

    // Buffer for distribute
    std::vector<int> clauses_to_distribute;

    if (_new_clauses.size() + _all_clauses.size() <= limit) {
        // All clauses can be send

        // Append new clauses to all clauses
        _all_clauses.insert(_all_clauses.end(), _new_clauses.begin(), _new_clauses.end());

        // Insert all clauses into distribute buffer
        clauses_to_distribute.insert(clauses_to_distribute.end(), _all_clauses.begin(), _all_clauses.end());

        // Reset start index
        _distribute_start_index = 0;

    } else if (_new_clauses.size() > limit) {
        // The new clauses are larger than the limit
        // This is very undesired! If this happens twice in a row some new clauses are not sent for some time
        // TODO Also gather is not restricted by size -> An error could happen because of to many clauses that are sent
        // Send as many new clauses as possible, then set the distribute start index in a way that the remaining clauses are sent next time
        _logger.log(0, "FailedAssumptionCommunicator: The new clauses are larger than the limit");

        // Set start index one behind the last element in all clauses
        int start_index = _all_clauses.size();

        // Append new clauses to all clauses
        _all_clauses.insert(_all_clauses.end(), _new_clauses.begin(), _new_clauses.end());

        std::vector<int>::const_iterator start_iterator = std::next(_all_clauses.cbegin(), start_index);

        std::vector<int>::const_iterator end_iterator = findEndIteratorForLimit(_all_clauses, start_index, limit);

        clauses_to_distribute.insert(clauses_to_distribute.cend(), start_iterator, end_iterator);

        // Set distribute start index to the first clause that was not send of the new clauses
        _distribute_start_index = end_iterator - _all_clauses.cbegin();

        assert(_all_clauses.at(_distribute_start_index) == *end_iterator);

    } else {
        // All new clauses can be send, but only a subset of the existing clauses

        // Remaining space in distribute buffer
        int remaining_space = limit - static_cast<int>(_new_clauses.size());
        // Must be larger than zero and smaller than then size of clauses
        assert(remaining_space > 0);
        assert(remaining_space < static_cast<int>(_all_clauses.size()));

        // The start index must point at the beginning of a clause
        assert(_all_clauses.at(_distribute_start_index) != 0);
        // Either the first clause or some clause in the buffer
        assert(_distribute_start_index == 0 || _all_clauses.at(_distribute_start_index - 1) == 0);
        // This clause is the first one to send

        // Create iterator from start index
        std::vector<int>::const_iterator start_iterator = std::next(_all_clauses.cbegin(), _distribute_start_index);

        assert(*start_iterator == _all_clauses.at(_distribute_start_index));

        // How many elements can be send before setting the start index back to the beginning
        int distance_until_end = std::distance(start_iterator, _all_clauses.cend());
        // Must be greater than or equal to two, since that is the smallest size for a clause
        assert(distance_until_end >= 2);

        if (distance_until_end == remaining_space) {
            // The elements between the start index and the end are exactly enough to fill the distribute buffer to its limit

            clauses_to_distribute.insert(clauses_to_distribute.end(), start_iterator, _all_clauses.cend());

            _distribute_start_index = 0;

        } else if (distance_until_end > remaining_space) {
            // The elements between the start index and the end are more than needed to fill the distribute buffer to its limit

            std::vector<int>::const_iterator end_iterator = findEndIteratorForLimit(_all_clauses, _distribute_start_index, remaining_space);

            clauses_to_distribute.insert(clauses_to_distribute.end(), start_iterator, end_iterator);

            // Set _distribute_start_index to index of end iterator
            _distribute_start_index = end_iterator - _all_clauses.begin();

            assert(_all_clauses.at(_distribute_start_index) == *end_iterator);

        } else {
            // The elements between the start index and the end are less than needed to fill the distribute buffer to its limit
            // Add them all and get the remaining from the beginning of _all_clauses
            assert(distance_until_end < remaining_space);

            // Inser all remaining
            clauses_to_distribute.insert(clauses_to_distribute.end(), start_iterator, _all_clauses.cend());

            // Recalculate remaining
            remaining_space = remaining_space - distance_until_end;

            // Remaining must be lower than the size
            assert(static_cast<int>(_all_clauses.size()) > remaining_space);

            std::vector<int>::const_iterator end_iterator = findEndIteratorForLimit(_all_clauses, 0, remaining_space);

            clauses_to_distribute.insert(clauses_to_distribute.end(), _all_clauses.cbegin(), end_iterator);

            // Set _distribute_start_index to index of end iterator
            _distribute_start_index = end_iterator - _all_clauses.begin();
        }
        // Insert new clauses into all clauses
        _all_clauses.insert(_all_clauses.end(), _new_clauses.begin(), _new_clauses.end());
    }
    // The entry condition for this branch is that there are either _new_clauses or _all_clauses is not empty -> something must be shared
    assert(clauses_to_distribute.size() > 0);
    assert(clauses_to_distribute.size() <= limit);

    // Distribute if there is something to distribute
    distribute(clauses_to_distribute);
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
    assert(_job.isActive() || _job.isSuspended() || _job.isCommitted() || Console::fail("%s", _job.jobStateToStr()));

    if (_job.isSuspended() || _job.isCommitted()) {
        // The call to suspend must already be finished
        assert(_received_failed_assumptions.empty());

        // The root node cannot be suspended
        assert(!_job.isRoot());

        // The possible message types in this situation
        assert(msg.tag == MSG_FAILED_ASSUMPTION_GATHER || msg.tag == MSG_FAILED_ASSUMPTION_DISTRIBUTE);

        // This message must be a remainder of before this job was suspended

        if (msg.tag == MSG_FAILED_ASSUMPTION_GATHER && !msg.payload.empty()) {
            _logger.log(0, "FailedAssumptionCommunicator: Received not empty gather while suspended");

            // Send the failed assumptions that would be lost to the root node
            sendToRoot(msg.payload);

        } else if (msg.tag == MSG_FAILED_ASSUMPTION_DISTRIBUTE) {
            assert(!msg.payload.empty());

            _logger.log(0, "FailedAssumptionCommunicator: Received distribute while suspended");
        }

        // Even when the job is suspended, new failed cubes may be learnt
        // if (msg.tag == MSG_FAILED_ASSUMPTION_DISTRIBUTE && _job.appl_doneInitializing()) {
        //     persist(msg.payload);
        // }
        // Currently MSG_FAILED_ASSUMPTION_DISTRIBUTE is ignored

    } else {
        // The job is active
        if (msg.tag == MSG_FAILED_ASSUMPTION_GATHER) {
            _logger.log(0, "FailedAssumptionCommunicator: Received gather while active");

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
            } else {
                _logger.log(0, "FailedAssumptionCommunicator: Received %i messages and has %i children", _messageCounter, numChildren);
            }

        } else if (msg.tag == MSG_FAILED_ASSUMPTION_DISTRIBUTE) {
            // The payload may not be empty
            assert(!msg.payload.empty());

            _logger.log(0, "FailedAssumptionCommunicator: Received distribute while active");

            // If the lib is initialized, persist the distributed failed assumptions
            if (_job.appl_doneInitializing()) persist(msg.payload);

            distribute(msg.payload);

        } else if (msg.tag == MSG_FAILED_ASSUMPTION_SEND_TO_ROOT) {
            // The payload may not be empty
            assert(!msg.payload.empty());

            // This message may only be send to the root node
            assert(_job.isRoot());

            _logger.log(0, "FailedAssumptionCommunicator: Received send to root");

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

void FailedAssumptionCommunicator::release() {
    // The caller guarantees that the DynamicCubeSatJob was interrupted

    // Reset messageCounter
    _messageCounter = 0;

    // Get local failed assumptions and insert into received failed assumptions
    auto failed_assumption = _job.getFailedAssumptions();
    _received_failed_assumptions.insert(_received_failed_assumptions.end(), failed_assumption.begin(), failed_assumption.end());

    if (!_received_failed_assumptions.empty()) {
        sendToRoot(_received_failed_assumptions);
    }

    // Clear everything
    _received_failed_assumptions.clear();
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
    std::string output = std::string(str) + " => [" + std::to_string(destRank) + "] Buffer size: " + std::to_string(payload.size());
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