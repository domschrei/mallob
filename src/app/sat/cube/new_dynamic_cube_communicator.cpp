#include "new_dynamic_cube_communicator.hpp"

#include <algorithm>
#include <cassert>
#include <iterator>
#include <sstream>

#include "comm/mympi.hpp"

NewDynamicCubeCommunicator::NewDynamicCubeCommunicator(DynamicCubeSatJob &job, LoggingInterface &logger, int cubesPerRequest)
    : _job(job), _logger(logger), _cubesPerRequest(cubesPerRequest) {}

void NewDynamicCubeCommunicator::sendMessageToParent() {
    // May be called during ACTIVE or INITIALIZING_TO_ACTIVE
    // If the second one is the case, the methods in the DynamicCubeSatJob guarantees that the uninitialized lib is not accessed and dummy values are returned
    assert(_job.isActive());

    _logger.log(0, "DynamicCubeCommunicator: Started sendMessageToParent");

    // Reset message counter
    _messageCounter = 0;

    // Find out if the corredponding job instance needs cubes
    bool isJobRequesting = _job.isRequesting();

    if (isJobRequesting) {
        // This node starts requesting

        // Node was not already requesting
        assert(!_isRequesting);

        _isRequesting = true;

        _logger.log(0, "DynamicCubeCommunicator: This job is requesting");

        // Add node rank to requester if not root
        if (!_job.isRoot()) _requester.push_back(_job.getMyMpiRank());
    }

    if (!_isRequesting && !_requester.empty()) {
        // This node is not requesting and has received requests
        satisfyRequests();
    }

    if (_job.isRoot()) {
        // I am root if i am requesting broadcastRootRequest
        if (_isRequesting) broadcastRootRequest();

    } else {
        // Non root nodes send the left over requests to their parent

        if (!_requester.empty()) {
            // There are still unsatisied requests -> Send them to the parent.
            sendRequestsToParent();

        } else {
            // Nothing left over -> Send all good to parent.
            sendAllGoodToParent();
        }

        // Now everything should be empty
        assert(_requester.empty());
    }
}

void NewDynamicCubeCommunicator::satisfyRequests() {
    // Calculate how many cubes are needed to fulfill each request with the specified amount of cubes per request
    int bias = static_cast<int>(_requester.size()) * _cubesPerRequest;
    std::vector<Cube> free_cubes = _job.getCubes(bias);

    _logger.log(0, "DynamicCubeCommunicator: There are %zu requester and %zu free cubes before satisfying", _requester.size(), free_cubes.size());

    // Spread the free cubes to the requester
    while (!free_cubes.empty()) {
        int requesterCount = static_cast<int>(_requester.size());
        int cubeCount = static_cast<int>(free_cubes.size());

        // Always shared at least one cube
        // If there is only one requester (left) it gets all remaining free cubes
        int cubesPerNextRequester = std::max(cubeCount / requesterCount, 1);

        // Extract cubes
        std::vector<Cube> cubesToSend;
        for (int i = 0; i < cubesPerNextRequester; i++) {
            assert(!free_cubes.empty());
            cubesToSend.push_back(free_cubes.back());
            free_cubes.pop_back();
        }

        // Send cubes
        fulfillRequest(_requester.back(), cubesToSend);

        // Remove requester
        _requester.pop_back();

        _logger.log(0, "DynamicCubeCommunicator: There are %zu requester and %zu free cubes after satisfying once", _requester.size(), free_cubes.size());
    }
}

void NewDynamicCubeCommunicator::fulfillRequest(int target, std::vector<Cube> &cubes) {
    JobMessage msg;
    msg.jobId = _job.getId();
    msg.epoch = 0;  // unused
    msg.tag = MSG_NEW_DYNAMIC_FULFILL;
    msg.payload = serializeCubes(cubes);

    log_send(target, msg.payload, "fulfillRequest");
    MyMpi::isend(MPI_COMM_WORLD, target, MSG_SEND_APPLICATION_MESSAGE, msg);
}

void NewDynamicCubeCommunicator::broadcastRootRequest() {
    JobMessage msg;
    msg.jobId = _job.getId();
    msg.epoch = 0;  // unused
    msg.tag = MSG_NEW_DYNAMIC_ROOT_REQUEST;

    if (_job.hasLeftChild()) {
        int leftChildRank = _job.getLeftChildNodeRank();
        log_send(leftChildRank, msg.payload, "broadcastRootRequest");
        MyMpi::isend(MPI_COMM_WORLD, leftChildRank, MSG_SEND_APPLICATION_MESSAGE, msg);
    }

    if (_job.hasRightChild()) {
        int rightChildRank = _job.getRightChildNodeRank();
        log_send(rightChildRank, msg.payload, "broadcastRootRequest");
        MyMpi::isend(MPI_COMM_WORLD, rightChildRank, MSG_SEND_APPLICATION_MESSAGE, msg);
    }
}

void NewDynamicCubeCommunicator::sendRequestsToParent() {
    JobMessage msg;
    msg.jobId = _job.getId();
    msg.epoch = 0;  // unused
    msg.tag = MSG_NEW_DYNAMIC_REQUEST;
    msg.payload = _requester;

    // Clear requester
    _requester.clear();

    int parentRank = _job.getParentNodeRank();
    log_send(parentRank, msg.payload, "sendRequestsToParent");
    MyMpi::isend(MPI_COMM_WORLD, parentRank, MSG_SEND_APPLICATION_MESSAGE, msg);
}

void NewDynamicCubeCommunicator::sendAllGoodToParent() {
    JobMessage msg;
    msg.jobId = _job.getId();
    msg.epoch = 0;  // unused
    msg.tag = MSG_NEW_DYNAMIC_ALL_GOOD;

    int parentRank = _job.getParentNodeRank();
    log_send(parentRank, msg.payload, "sendAllGoodToParent");
    MyMpi::isend(MPI_COMM_WORLD, parentRank, MSG_SEND_APPLICATION_MESSAGE, msg);
}

void NewDynamicCubeCommunicator::handle(int source, JobMessage &msg) {
    // Caller guarantees that the job cannot be interrupted

    // Can only be suspended or active, because the message was once in the job tree
    assert(_job.isActive() || _job.isSuspended() || _job.isCommitted() || Console::fail("%s", _job.jobStateToStr()));

    if (_job.isSuspended() || _job.isCommitted()) {
        // The call to suspend must already be finished
        assert(_requester.empty());
        assert(!_isRequesting);

        // The root node cannot be suspended
        // Also it must be working, otherwise the demand was 1
        assert(!_job.isRoot());

        // The possible message types in this situation
        assert(msg.tag == MSG_NEW_DYNAMIC_REQUEST || msg.tag == MSG_NEW_DYNAMIC_ALL_GOOD || msg.tag == MSG_NEW_DYNAMIC_FULFILL || MSG_NEW_DYNAMIC_ROOT_REQUEST);

        // This message must be a remainder of before this job was suspended
        if (msg.tag == MSG_NEW_DYNAMIC_FULFILL) {
            // The payload may not be empty
            assert(!msg.payload.empty());

            _logger.log(0, "DynamicCubeCommunicator: Received fulfill while suspended");

            // Send the cubes that would be lost to the root node
            auto cubes = unserializeCubes(msg.payload);
            sendCubesToRoot(cubes);
        } else {
            // MSG_NEW_DYNAMIC_REQUEST, MSG_NEW_DYNAMIC_ALL_GOOD and MSG_NEW_DYNAMIC_ROOT_REQUEST are ignored
            _logger.log(0, "DynamicCubeCommunicator: Received request, all good or root request while suspended");
        }

    } else {
        // The job is active
        if (msg.tag == MSG_NEW_DYNAMIC_REQUEST || msg.tag == MSG_NEW_DYNAMIC_ALL_GOOD) {
            if (msg.tag == MSG_NEW_DYNAMIC_REQUEST) {
                // The payload may not be empty
                assert(!msg.payload.empty());

                _logger.log(0, "DynamicCubeCommunicator: Received request while active");

                // Insert received requester
                _requester.insert(_requester.end(), msg.payload.begin(), msg.payload.end());

            } else if (msg.tag == MSG_NEW_DYNAMIC_ALL_GOOD) {
                _logger.log(0, "DynamicCubeCommunicator: Received all good while active");
            }

            // Count children
            int numChildren = 0;
            if (_job.hasLeftChild()) numChildren++;
            if (_job.hasRightChild()) numChildren++;

            if (++_messageCounter >= numChildren) {
                sendMessageToParent();
            } else {
                _logger.log(0, "DynamicCubeCommunicator: Received %i messages and has %i children", _messageCounter, numChildren);
            }

        } else if (msg.tag == MSG_NEW_DYNAMIC_FULFILL) {
            // The payload may not be empty and receiver may not be root
            assert(!msg.payload.empty());
            assert(!_job.isRoot());

            // Set to not requesting, do not assert(_isRequesting) since a node may receive a fulfill from before the last suspension
            _isRequesting = false;

            _logger.log(0, "DynamicCubeCommunicator: Received fulfill while active");

            auto cubes = unserializeCubes(msg.payload);
            _job.digestCubes(cubes);

        } else if (msg.tag == MSG_NEW_DYNAMIC_ROOT_REQUEST) {
            _logger.log(0, "DynamicCubeCommunicator: Received root request while active");

            // Try to get free cubes
            std::vector<Cube> free_cubes = _job.getCubes(_cubesPerRequest);

            if (!free_cubes.empty()) {
                sendCubesToRoot(free_cubes);
            } else {
                broadcastRootRequest();
            }

        } else if (msg.tag == MSG_NEW_DYNAMIC_SEND_TO_ROOT) {
            // The payload may not be empty
            assert(!msg.payload.empty());

            // This message may only be send to the root node
            assert(_job.isRoot());

            // Set to not requesting
            _isRequesting = false;

            _logger.log(0, "DynamicCubeCommunicator: Root received send to root");

            // Digest cubes
            auto cubes = unserializeCubes(msg.payload);
            _job.digestCubes(cubes);
        }
    }
}

void NewDynamicCubeCommunicator::sendCubesToRoot(std::vector<Cube> &cubes) {
    JobMessage msg;
    msg.jobId = _job.getId();
    msg.epoch = 0;  // unused
    msg.tag = MSG_NEW_DYNAMIC_SEND_TO_ROOT;
    msg.payload = serializeCubes(cubes);

    int rootRank = _job.getRootNodeRank();
    log_send(rootRank, msg.payload, "sendCubesToRoot");
    MyMpi::isend(MPI_COMM_WORLD, rootRank, MSG_SEND_APPLICATION_MESSAGE, msg);
}

void NewDynamicCubeCommunicator::releaseAll() {
    // The caller guarantees that the DynamicCubeSatJob was interrupted and the cubes were unassigned

    // Reset messageCounter
    _messageCounter = 0;

    // Set to no requesting
    _isRequesting = false;

    auto cubes = _job.releaseAllCubes();

    if (!cubes.empty()) {
        sendCubesToRoot(cubes);
    }

    // Clear everything
    _requester.clear();
}

bool NewDynamicCubeCommunicator::isDynamicCubeMessage(int tag) {
    return tag == MSG_NEW_DYNAMIC_REQUEST || tag == MSG_NEW_DYNAMIC_ALL_GOOD || tag == MSG_NEW_DYNAMIC_FULFILL ||
           tag == MSG_NEW_DYNAMIC_ROOT_REQUEST || tag == MSG_NEW_DYNAMIC_SEND_TO_ROOT;
}

void NewDynamicCubeCommunicator::log_send(int destRank, std::vector<int> &payload, const char *str, ...) {
    // // Convert payload
    // auto payloadString = payloadToString(payload);

    // For addtitional printf params
    va_list vl;
    // Puts first value in parameter list into str
    va_start(vl, str);
    std::string output = std::string(str) + " => [" + std::to_string(destRank) + "] Buffer size: " + std::to_string(payload.size());
    _logger.log_va_list(0, output.c_str(), vl);
    va_end(vl);
}

std::string NewDynamicCubeCommunicator::payloadToString(std::vector<int> &payload) {
    // https://www.geeksforgeeks.org/transform-vector-string/
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