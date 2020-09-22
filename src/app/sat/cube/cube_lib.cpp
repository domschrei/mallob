#include "cube_lib.hpp"

#include <cassert>

#include "cube_communicator.hpp"
#include "util/console.hpp"

CubeLib::CubeLib(std::vector<int> formula, CubeCommunicator &cube_comm) : _formula(formula) {
    _cube_worker = std::make_unique<CubeWorker>(_formula, cube_comm, _result);
}

CubeLib::CubeLib(std::vector<int> formula, CubeCommunicator &cube_comm, int depth, size_t cubes_per_worker) : CubeLib(formula, cube_comm) {
    _cube_root = std::make_unique<CubeRoot>(_formula, cube_comm, _result, depth, cubes_per_worker);
    _isRoot = true;
}

void CubeLib::generateCubes() {
    _cube_root->generateCubes();
}

void CubeLib::startWorking() {
    _cube_worker->startWorking();
}

void CubeLib::interrupt() {
    _isInterrupted.store(true);
    _cube_worker->interrupt();
}

void CubeLib::withdraw() {
    _cube_worker->join();
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
        if (_isRoot && (msg.tag == MSG_REQUEST_CUBES || msg.tag == MSG_RETURN_FAILED_CUBES)) {
            _cube_root->handleMessage(source, msg);
        } else if (msg.tag == MSG_SEND_CUBES || msg.tag == MSG_RECEIVED_FAILED_CUBES) {
            _cube_worker->handleMessage(source, msg);
        }
        // TODO: Throw error
    }
}
