#include "dynamic_cube_communicator.hpp"

#include <algorithm>
#include <cassert>
#include <iterator>
#include <sstream>

#include "comm/mympi.hpp"

DynamicCubeCommunicator::DynamicCubeCommunicator(DynamicCubeSatJob &job, LoggingInterface &logger) : _job(job), _logger(logger){};

void DynamicCubeCommunicator::sendMessageToParent() {
    // May be called during ACTIVE or INITIALIZING_TO_ACTIVE
    // If the second one is the case, the methods in the DynamicCubeSatJob guarantees that the uninitialized lib is not accessed and dummy values are returned
    assert(_job.isActive());

    // Reset message counter
    _messageCounter = 0;

    // Find out if the corredponding job instance needs cubes
    bool isRequesting = _job.isRequesting();

    if (isRequesting) {
        // TODO Find which is the correct field here
        _requester.push_back(_job.getMyMpiRank());

    } else {
        // Try to get cubes. This is biased by the received requests and cubes
        size_t bias = _requester.size() * _cubesPerRequest - _received_cubes.size();
        std::vector<Cube> cubes = _job.getCubes(bias);
        _received_cubes.insert(_received_cubes.end(), cubes.begin(), cubes.end());
    }

    // Satisfy nodes while both requester and received cubes is set
    while (!_requester.empty() && !_received_cubes.empty()) {
        size_t requesterCount = _requester.size();
        size_t cubeCount = _received_cubes.size();

        // Calculate min of ceiled division and maximum cubes per request
        size_t cubesPerNextRequester = std::min(cubeCount / requesterCount + (cubeCount % requesterCount != 0), _cubesPerRequest);

        // Extract cubes
        std::vector<Cube> cubesToSend;
        for (size_t i = 0; i < cubesPerNextRequester; i++) {
            assert(!_received_cubes.empty());
            cubesToSend.push_back(_received_cubes.back());
            _received_cubes.pop_back();
        }

        // Send cubes
        fulfillRequest(_requester.back(), cubesToSend);

        // Remove requester
        _requester.pop_back();
    }

    // Now either requester or/and received cubes should be empty
    assert(_requester.empty() || _received_cubes.empty());

    // Stop here if i am root
    if (_job.isRoot()) return;

    // Non root nodes send the left over information to their parent
    if (!_received_cubes.empty()) {
        // There are still free cubes left over. Send them to the parent.
        sendCubesToParent();

    } else if (!_requester.empty()) {
        // There are still unsatisied requests. Send them to the parent.
        sendRequestsToParent();

    } else {
        // Nothing left over. Send all good to parent.
        sendAllGoodToParent();
    }

    // Now everything should be empty
    assert(_requester.empty() && _received_cubes.empty());
};

void DynamicCubeCommunicator::fulfillRequest(int target, std::vector<Cube> &cubes) {
    JobMessage msg;
    msg.jobId = _job.getId();
    msg.epoch = 0;  // unused
    msg.tag = MSG_DYNAMIC_FULFILL;
    msg.payload = serializeCubes(cubes);

    log_send(target, msg.payload, "fulfillRequest");
    MyMpi::isend(MPI_COMM_WORLD, target, MSG_SEND_APPLICATION_MESSAGE, msg);
}

void DynamicCubeCommunicator::sendCubesToParent() {
    JobMessage msg;
    msg.jobId = _job.getId();
    msg.epoch = 0;  // unused
    msg.tag = MSG_DYNAMIC_SEND;
    msg.payload = serializeCubes(_received_cubes);

    // Clear received cubes
    _received_cubes.clear();

    int parentRank = _job.getParentNodeRank();
    log_send(parentRank, msg.payload, "sendCubesToParent");
    MyMpi::isend(MPI_COMM_WORLD, parentRank, MSG_SEND_APPLICATION_MESSAGE, msg);
};

void DynamicCubeCommunicator::sendRequestsToParent() {
    JobMessage msg;
    msg.jobId = _job.getId();
    msg.epoch = 0;  // unused
    msg.tag = MSG_DYNAMIC_REQUEST;
    msg.payload = _requester;

    // Clear requester
    _requester.clear();

    int parentRank = _job.getParentNodeRank();
    log_send(parentRank, msg.payload, "sendRequestsToParent");
    MyMpi::isend(MPI_COMM_WORLD, parentRank, MSG_SEND_APPLICATION_MESSAGE, msg);
};

void DynamicCubeCommunicator::sendAllGoodToParent() {
    JobMessage msg;
    msg.jobId = _job.getId();
    msg.epoch = 0;  // unused
    msg.tag = MSG_DYNAMIC_ALL_GOOD;

    int parentRank = _job.getParentNodeRank();
    log_send(parentRank, msg.payload, "sendAllGoodToParent");
    MyMpi::isend(MPI_COMM_WORLD, parentRank, MSG_SEND_APPLICATION_MESSAGE, msg);
};

void DynamicCubeCommunicator::handle(int source, JobMessage &msg) {
    // Caller guarantees that the job cannot be interrupted

    // Can only be suspended or active, because the message was once in the job tree
    assert(_job.isSuspended() || _job.isActive());

    if (_job.isSuspended()) {
        // The call to suspend must already be finished
        assert(_received_cubes.empty() && _requester.empty());

        // The root node cannot be suspended
        assert(!_job.isRoot());

        // The possible message types in this situation
        assert(msg.tag == MSG_DYNAMIC_SEND || msg.tag == MSG_DYNAMIC_REQUEST || msg.tag == MSG_DYNAMIC_ALL_GOOD || msg.tag == MSG_DYNAMIC_FULFILL);

        // This message must be a remainder of before this job was suspended
        if (msg.tag == MSG_DYNAMIC_SEND || msg.tag == MSG_DYNAMIC_FULFILL) {
            // Send the cubes that would be lost to the root node
            auto cubes = unserializeCubes(msg.payload);
            sendCubesToRoot(cubes);
        }

        // MSG_DYNAMIC_REQUEST and MSG_DYNAMIC_ALL_GOOD are ignored

    } else {
        // The job is active
        if (msg.tag == MSG_DYNAMIC_SEND || msg.tag == MSG_DYNAMIC_REQUEST || msg.tag == MSG_DYNAMIC_ALL_GOOD) {
            if (msg.tag == MSG_DYNAMIC_SEND) {
                // The payload may not be empty
                assert(!msg.payload.empty());

                // Insert received cubes
                auto cubes = unserializeCubes(msg.payload);
                _received_cubes.insert(_received_cubes.end(), cubes.begin(), cubes.end());

            } else if (msg.tag == MSG_DYNAMIC_REQUEST) {
                // The payload may not be empty
                assert(!msg.payload.empty());

                // Insert received requester
                _requester.insert(_requester.end(), msg.payload.begin(), msg.payload.end());
            }

            // Count children
            int numChildren = 0;
            if (_job.hasLeftChild()) numChildren++;
            if (_job.hasRightChild()) numChildren++;

            if (++_messageCounter >= numChildren) {
                sendMessageToParent();
            }

        } else if (msg.tag == MSG_DYNAMIC_FULFILL) {
            // The payload may not be empty
            assert(!msg.payload.empty());

            auto cubes = unserializeCubes(msg.payload);
            _job.digestCubes(cubes);

        } else if (msg.tag == MSG_DYNAMIC_SEND_TO_ROOT) {
            // The payload may not be empty
            assert(!msg.payload.empty());

            // This message may only be send to the root node
            assert(_job.isRoot());

            auto cubes = unserializeCubes(msg.payload);
            _received_cubes.insert(_received_cubes.end(), cubes.begin(), cubes.end());
        }
    }
};

void DynamicCubeCommunicator::sendCubesToRoot(std::vector<Cube> &cubes) {
    JobMessage msg;
    msg.jobId = _job.getId();
    msg.epoch = 0;  // unused
    msg.tag = MSG_DYNAMIC_SEND_TO_ROOT;
    msg.payload = serializeCubes(cubes);

    int rootRank = _job.getRootNodeRank();
    log_send(rootRank, msg.payload, "sendCubesToRoot");
    MyMpi::isend(MPI_COMM_WORLD, rootRank, MSG_SEND_APPLICATION_MESSAGE, msg);
}

void DynamicCubeCommunicator::releaseAll() {
    // The caller guarantees that the DynamicCubeSatJob was interrupted and the cubes were unassigned

    auto cubes = _job.releaseAllCubes();

    // Insert released cubes into received cubes
    _received_cubes.insert(_received_cubes.end(), cubes.begin(), cubes.end());

    if (!_received_cubes.empty()) {
        sendCubesToRoot(_received_cubes);
    }

    // Clear everything
    _received_cubes.clear();
    _requester.clear();
}

void DynamicCubeCommunicator::log_send(int destRank, std::vector<int> &payload, const char *str, ...) {
    // Convert payload
    auto payloadString = payloadToString(payload);

    // For addtitional printf params
    va_list vl;
    // Puts first value in parameter list into str
    va_start(vl, str);
    std::string output = std::string(str) + " => [" + std::to_string(destRank) + "] {" + payloadString + "}";
    _logger.log_va_list(0, output.c_str(), vl);
    va_end(vl);
}

std::string DynamicCubeCommunicator::payloadToString(std::vector<int> &payload) {
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