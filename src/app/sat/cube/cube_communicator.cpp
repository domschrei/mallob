#include "cube_communicator.hpp"

#include <cassert>
#include <iterator>
#include <sstream>

#include "comm/mympi.hpp"

void CubeCommunicator::requestCubes() {
    // Send request message to root node
    JobMessage msg;
    msg.jobId = _job.getId();
    msg.epoch = 0;  // unused
    msg.tag = MSG_REQUEST_CUBES;

    int rootRank = _job.getRootNodeRank();
    log_send(rootRank, msg.payload, "requestCubes");
    MyMpi::isend(MPI_COMM_WORLD, rootRank, MSG_SEND_APPLICATION_MESSAGE, msg);
}

void CubeCommunicator::requestCubesFromParent() {
    // Send request message to parent node
    JobMessage msg;
    msg.jobId = _job.getId();
    msg.epoch = 0;  // unused
    msg.tag = MSG_REQUEST_CUBES_FROM_PARENT;

    int parentRank = _job.getParentNodeRank();
    log_send(parentRank, msg.payload, "requestCubesFromParent");
    MyMpi::isend(MPI_COMM_WORLD, parentRank, MSG_SEND_APPLICATION_MESSAGE, msg);
}

void CubeCommunicator::sendCubes(int target, std::vector<int> &serialized_cubes) {
    assert(!serialized_cubes.empty());

    // Send cubes to requesting node
    JobMessage msg;
    msg.jobId = _job.getId();
    msg.epoch = 0;  // unused
    msg.tag = MSG_SEND_CUBES;
    msg.payload = serialized_cubes;

    log_send(target, msg.payload, "sendCubes");
    MyMpi::isend(MPI_COMM_WORLD, target, MSG_SEND_APPLICATION_MESSAGE, msg);
}

void CubeCommunicator::returnFailedCubes(std::vector<int> &serialized_failed_cubes) {
    assert(!serialized_failed_cubes.empty());

    // Return finished cubes to requesting node
    JobMessage msg;
    msg.jobId = _job.getId();
    msg.epoch = 0;  // unused
    msg.tag = MSG_RETURN_FAILED_CUBES;
    msg.payload = serialized_failed_cubes;

    int rootRank = _job.getRootNodeRank();
    log_send(rootRank, msg.payload, "returnFailedCubes");
    MyMpi::isend(MPI_COMM_WORLD, rootRank, MSG_SEND_APPLICATION_MESSAGE, msg);
}

void CubeCommunicator::receivedFailedCubes(int target) {
    // Notify worker that failed cubes were received
    JobMessage msg;
    msg.jobId = _job.getId();
    msg.epoch = 0;  // unused
    msg.tag = MSG_RECEIVED_FAILED_CUBES;

    log_send(target, msg.payload, "receivedFailedCubes");
    MyMpi::isend(MPI_COMM_WORLD, target, MSG_SEND_APPLICATION_MESSAGE, msg);
}

void CubeCommunicator::returnFailedAndRequestCubes(std::vector<int> &serialized_failed_cubes) {
    // Return possibly empty failed cubes to root and request new cubes
    JobMessage msg;
    msg.jobId = _job.getId();
    msg.epoch = 0;  // unused
    msg.tag = MSG_RETURN_FAILED_AND_REQUEST_CUBES;
    msg.payload = serialized_failed_cubes;

    int rootRank = _job.getRootNodeRank();
    log_send(rootRank, msg.payload, "returnFailedAndRequestCubes");
    MyMpi::isend(MPI_COMM_WORLD, rootRank, MSG_SEND_APPLICATION_MESSAGE, msg);
}

void CubeCommunicator::requestFailedCubes() {
    // Send request message to root node
    JobMessage msg;
    msg.jobId = _job.getId();
    msg.epoch = 0;  // unused
    msg.tag = MSG_REQUEST_FAILED_CUBES;

    int rootRank = _job.getRootNodeRank();
    log_send(rootRank, msg.payload, "requestFailedCubes");
    MyMpi::isend(MPI_COMM_WORLD, rootRank, MSG_SEND_APPLICATION_MESSAGE, msg);
}

void CubeCommunicator::sendFailedCubes(int target, std::vector<int>& serialized_failed_cubes) {
    // Send (possibly empty) failed cubes to requesting node
    // Empty because this kind u message is necessaryto receive to finish the initialization
    JobMessage msg;
    msg.jobId = _job.getId();
    msg.epoch = 0;  // unused
    msg.tag = MSG_SEND_FAILED_CUBES;
    msg.payload = serialized_failed_cubes;

    log_send(target, msg.payload, "sendFailedCubes");
    MyMpi::isend(MPI_COMM_WORLD, target, MSG_SEND_APPLICATION_MESSAGE, msg);
}

void CubeCommunicator::sendCubesToRoot(std::vector<int> &serialized_cubes) {
    assert(!serialized_cubes.empty());

    // Send cubes to requesting node
    JobMessage msg;
    msg.jobId = _job.getId();
    msg.epoch = 0;  // unused
    msg.tag = MSG_SEND_CUBES;
    msg.payload = serialized_cubes;

    int rootRank = _job.getRootNodeRank();
    log_send(rootRank, msg.payload, "sendCubesToRoot");
    MyMpi::isend(MPI_COMM_WORLD, rootRank, MSG_SEND_APPLICATION_MESSAGE, msg);
}

void CubeCommunicator::log_send(int destRank, std::vector<int> &payload, const char *str, ...) {
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

std::string CubeCommunicator::payloadToString(std::vector<int> &payload) {
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