#include "util/console.hpp"
#include "comm/mympi.hpp"

#include "cube_communicator.hpp"

void CubeCommunicator::requestCubes() {
    Console::log(Console::INFO, "Reached request function");
    // Send request message to root node
    JobMessage msg;
    msg.jobId = _job->getId();
    msg.epoch = 0; // unused
    msg.tag = MSG_REQUEST_CUBES;

    int rootRank = _job->getRootNodeRank();
    Console::log_send(Console::INFO, rootRank, "%s : requestCubes", _job->toStr());
    MyMpi::isend(MPI_COMM_WORLD, rootRank, MSG_SEND_APPLICATION_MESSAGE, msg);
}

void CubeCommunicator::sendCubes(int target) {
    Console::log(Console::INFO, "Reached send function");
    // Send cubes to requesting node
    JobMessage msg;
    msg.jobId = _job->getId();
    msg.epoch = 0; // unused
    msg.tag = MSG_SEND_CUBES;
    msg.payload = _job->getPreparedCubes();

    Console::log_send(Console::VERB, target, "%s : sendCubes", _job->toStr());
    MyMpi::isend(MPI_COMM_WORLD, target, MSG_SEND_APPLICATION_MESSAGE, msg);
};
    
void CubeCommunicator::handle(int source, JobMessage& msg) {
    if (!_initialized || _job->isNotInState({JobState::ACTIVE}))
        return;

    if (msg.tag == MSG_REQUEST_CUBES) {
        if (!_job->isRoot())
            Console::fail("Not root received Request for Cubes");

        sendCubes(source);
    } 
    else if (msg.tag == MSG_SEND_CUBES) {
        _job->digestCubes(msg.payload);
    }
};