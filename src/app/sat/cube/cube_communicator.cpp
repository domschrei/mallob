#include "cube_communicator.hpp"

#include "comm/mympi.hpp"
#include "util/console.hpp"

void CubeCommunicator::requestCubes() {
    // Send request message to root node
    JobMessage msg;
    msg.jobId = _job->getId();
    msg.epoch = 0;  // unused
    msg.tag = MSG_REQUEST_CUBES;

    int rootRank = _job->getRootNodeRank();
    Console::log_send(Console::INFO, rootRank, "%s : requestCubes", _job->toStr());
    MyMpi::isend(MPI_COMM_WORLD, rootRank, MSG_SEND_APPLICATION_MESSAGE, msg);
}

void CubeCommunicator::sendCubes(int target, std::vector<int> &serialized_cubes) {
    // Send cubes to requesting node
    JobMessage msg;
    msg.jobId = _job->getId();
    msg.epoch = 0;  // unused
    msg.tag = MSG_SEND_CUBES;
    msg.payload = serialized_cubes;

    Console::log_send(Console::INFO, target, "%s : sendCubes", _job->toStr());
    MyMpi::isend(MPI_COMM_WORLD, target, MSG_SEND_APPLICATION_MESSAGE, msg);
}

void CubeCommunicator::returnFailedCubes(std::vector<int> &serialized_failed_cubes) {
    // Return finished cubes to requesting node
    JobMessage msg;
    msg.jobId = _job->getId();
    msg.epoch = 0;  // unused
    msg.tag = MSG_RETURN_FAILED_CUBES;
    msg.payload = serialized_failed_cubes;

    int rootRank = _job->getRootNodeRank();
    Console::log_send(Console::INFO, rootRank, "%s : returnFailedCubes", _job->toStr());
    MyMpi::isend(MPI_COMM_WORLD, rootRank, MSG_SEND_APPLICATION_MESSAGE, msg);
}

void CubeCommunicator::receivedFailedCubes(int target) {
    // Notify worker that failed cubes were received
    JobMessage msg;
    msg.jobId = _job->getId();
    msg.epoch = 0;  // unused
    msg.tag = MSG_RECEIVED_FAILED_CUBES;

    Console::log_send(Console::INFO, target, "%s : receivedFailedCubes", _job->toStr());
    MyMpi::isend(MPI_COMM_WORLD, target, MSG_SEND_APPLICATION_MESSAGE, msg);
}
