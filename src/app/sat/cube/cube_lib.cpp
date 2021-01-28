#include "cube_lib.hpp"

#include <cassert>

#include "cube_communicator.hpp"
#include "cube_worker.hpp"
#include "cube_worker_greedy.hpp"
#include "util/console.hpp"

CubeLib::CubeLib(CubeSetup &setup) {
    if (setup.params.getParam("cube-worker") == "greedy")
        _cube_worker = std::make_unique<CubeWorkerGreedy>(setup);
    else
        _cube_worker = std::make_unique<CubeWorker>(setup);

    _cube_root = std::make_unique<CubeRoot>(setup);
}

CubeLib::~CubeLib() {}

bool CubeLib::generateCubes() {
    return _cube_root->generateCubes();
}

void CubeLib::startWorking() {
    _cube_worker->startWorking();
}

// Interrupt is called sequentially to wantsToCommunicate, beginCommunication and handleMessage.
// So there is no communication after a call to interrupt.
// Only two flags are set so this return fast.
void CubeLib::interrupt() {
    _isInterrupted.store(true);
    if (_cube_root != nullptr) _cube_root->interrupt();
    if (_cube_worker != nullptr) _cube_worker->interrupt();
}

// Waits for worker threads to finish.
void CubeLib::withdraw() {
    _cube_worker->join();
}

void CubeLib::suspend() {
    _cube_worker->suspend();
}

void CubeLib::resume() {
    _cube_worker->resume();
}

// Only the worker starts communication. Execution only needs to be passed through.
bool CubeLib::wantsToCommunicate() {
    if (!_isInterrupted)
        return _cube_worker->wantsToCommunicate();
    else
        return false;
}

// Only the worker starts communication. Execution only needs to be passed through.
void CubeLib::beginCommunication() {
    if (!_isInterrupted)
        _cube_worker->beginCommunication();
}

// Pass the message to either the root or the worker
void CubeLib::handleMessage(int source, JobMessage &msg) {
    if (!_isInterrupted) {
        if (msg.tag == MSG_REQUEST_CUBES || msg.tag == MSG_RETURN_FAILED_CUBES || msg.tag == MSG_RETURN_FAILED_AND_REQUEST_CUBES) {
            _cube_root->handleMessage(source, msg);
        } else if (msg.tag == MSG_SEND_CUBES || msg.tag == MSG_RECEIVED_FAILED_CUBES) {
            _cube_worker->handleMessage(source, msg);
        }
        // TODO: Throw error
    }
}
