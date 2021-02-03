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
        // Persist everything received
        persist(_received_failed_assumptions);

        // Clear received failed assumptions
        _received_failed_assumptions.clear();

        // Start distribute if there are new clauses or existing clauses
        if (!_new_clauses.empty() || !_all_clauses.empty()) {
            // We set a fix limit for distribute
            // Every distribute we share the new clauses
            // Then we fill the buffer using old clauses, starting from the member _distribute_start_index
            const int limit = 10000;
            // TODO Rework using dynamic limit from AnyTimeSatClauseCommunicator

            // Buffer for distribute
            std::vector<int> clauses_to_distribute;

            // Insert new clauses from last persist
            // If there are no new clauses, none are inserted
            clauses_to_distribute.insert(clauses_to_distribute.end(), _new_clauses.begin(), _new_clauses.end());

            if (_new_clauses.size() + _all_clauses.size() <= limit) {
                // All clauses can be send

                // Reset start index
                _distribute_start_index = 0;

                // Insert all clauses into distribute buffer
                clauses_to_distribute.insert(clauses_to_distribute.end(), _all_clauses.begin(), _all_clauses.end());

            } else {
                // Not all clauses can be send

                // Remaining space in distribute buffer
                int remaining = limit - static_cast<int>(_new_clauses.size());
                // Must be larger than zero
                assert(remaining >= 1);

                // The start index must point at the beginning of a clause
                assert(_all_clauses.at(_distribute_start_index) != 0);
                assert(_distribute_start_index == 0 || _all_clauses.at(_distribute_start_index - 1) == 0);
                // This clause is the first one to send

                // Create iterator from start index
                std::vector<int>::iterator start_iterator = std::next(_all_clauses.begin(), _distribute_start_index);

                assert(*start_iterator == _all_clauses.at(_distribute_start_index));

                // How many elements can be send before setting the start index back to the beginning
                int size_until_end = std::distance(start_iterator, _all_clauses.end());
                // Must be greater than or equal to two, since that is the smallest size for a clause
                assert(size_until_end >= 2);

                if (size_until_end == remaining) {
                    // The elements between the start index and the end are exactly enough to fill the distribute buffer to its limit

                    clauses_to_distribute.insert(clauses_to_distribute.end(), start_iterator, _all_clauses.end());

                    _distribute_start_index = 0;

                } else if (size_until_end > remaining) {
                    // The elements between the start index and the end are more than needed to fill the distribute buffer to its limit

                    // This reverse iterator points inclusively between the same element of start_iterator or at the second to last element of _all_clauses
                    std::vector<int>::reverse_iterator reverse_end_iterator = std::next(_all_clauses.rbegin(), size_until_end - remaining);

                    // Find closest zero or the end
                    reverse_end_iterator = std::find(reverse_end_iterator, _all_clauses.rend(), 0);

                    assert(reverse_end_iterator == _all_clauses.rend() || *reverse_end_iterator == 0);

                    // Create iterator one element behind reverse_end_iterator
                    // https://riptutorial.com/cplusplus/example/5101/reverse-iterators
                    std::vector<int>::iterator end_iterator = reverse_end_iterator.base();

                    assert(end_iterator != _all_clauses.end());
                    assert(std::distance(start_iterator, end_iterator) < remaining);

                    clauses_to_distribute.insert(clauses_to_distribute.end(), start_iterator, end_iterator);

                    // Set _distribute_start_index to index of end iterator
                    _distribute_start_index = end_iterator - _all_clauses.begin();

                } else {
                    // The elements between the start index and the end are less than needed to fill the distribute buffer to its limit
                    // Add them all and get the remaining from the beginning of _all_clauses

                    clauses_to_distribute.insert(clauses_to_distribute.end(), start_iterator, _all_clauses.end());

                    // Recalculate remaining
                    remaining = remaining - size_until_end;

                    // Remaining must be lower than the size
                    assert(static_cast<int>(_all_clauses.size()) > remaining);
                    // This reverse iterator points at the maximum amount of elements that are in the limit
                    std::vector<int>::reverse_iterator reverse_end_iterator = std::next(_all_clauses.rbegin(), _all_clauses.size() - remaining);

                    // Find closest zero or the end
                    reverse_end_iterator = std::find(reverse_end_iterator, _all_clauses.rend(), 0);

                    assert(reverse_end_iterator == _all_clauses.rend() || *reverse_end_iterator == 0);

                    // Create iterator one element behind reverse_end_iterator
                    // https://riptutorial.com/cplusplus/example/5101/reverse-iterators
                    std::vector<int>::iterator end_iterator = reverse_end_iterator.base();

                    clauses_to_distribute.insert(clauses_to_distribute.end(), _all_clauses.begin(), end_iterator);

                    // Set _distribute_start_index to index of end iterator
                    _distribute_start_index = end_iterator - _all_clauses.begin();
                }
            }

            assert(clauses_to_distribute.size() <= limit);

            // Distribute if there is something to distribute
            if (!clauses_to_distribute.empty()) {
                distribute(clauses_to_distribute);
            }

            // Insert new clauses into all clauses
            _all_clauses.insert(_all_clauses.end(), _new_clauses.begin(), _new_clauses.end());
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