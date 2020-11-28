#include "dynamic_cube_lib.hpp" 

#include <cassert>
#include <iterator>

#include "app/sat/console_horde_interface.hpp"
#include "app/sat/hordesat/solvers/cadical.hpp"
#include "app/sat/hordesat/solvers/cadical_interface.hpp"
#include "cube_communicator.hpp"
#include "util/console.hpp"

DynamicCubeLib::DynamicCubeLib(CubeSetup &setup) {
    _solver_thread_count = 2;
    _generator_thread_count = 1;

    // Create cube solver threads
    for (size_t i = 0; i < _solver_thread_count; i++) {
        // https://stackoverflow.com/questions/3283778/why-can-i-not-push-back-a-unique-ptr-into-a-vector
        std::unique_ptr<CubeSolverThread> solver = std::make_unique<CubeSolverThread>(*this, setup);
        _solver_threads.push_back(std::move(solver));
    }

    // Create cube generator threads
    for (size_t i = 0; i < _generator_thread_count; i++) {
        // https://stackoverflow.com/questions/3283778/why-can-i-not-push-back-a-unique-ptr-into-a-vector
        std::unique_ptr<CubeGeneratorThread> generator = std::make_unique<CubeGeneratorThread>(*this, setup);
        _generator_threads.push_back(std::move(generator));
    }
}

void DynamicCubeLib::startWorking() {
    _state.store(INIT_REQUESTING);
}

bool DynamicCubeLib::wantsToCommunicateImpl() {
    if (_state == INIT_REQUESTING)
        return true
    else 
        return false;
}

void DynamicCubeLib::beginCommunication() {
    const std::lock_guard<Mutex> lock(_local_cubes_mutex);

    auto serialized_failed_cubes = serializeCubes(_failed_local_cubes);

    _cube_comm.returnFailedAndRequestCubes(serialized_failed_cubes);

    _logger.log(0, "Sent %zu failed cubes to root and requested new cubes", _failed_local_cubes.size());

    _request_state = RECEIVING_CUBES;

    _failed_local_cubes.clear();
}

void CubeWorkerGreedy::handleMessage(int source, JobMessage &msg) {
    const std::lock_guard<Mutex> lock(_local_cubes_mutex);

    if (msg.tag == MSG_SEND_CUBES) {
        auto serialized_cubes = msg.payload;
        auto cubes = unserializeCubes(serialized_cubes);

        _logger.log(0, "Received %zu cubes from root", cubes.size());

        digestSendCubes(cubes);

    } else {
        // TODO: Throw error
    }
}

void CubeWorkerGreedy::digestSendCubes(std::vector<Cube> &cubes) {
    assert(_request_state == RECEIVING_CUBES);

    // Prune received cubes using the failed cubes that were received during communication
    prune(cubes, _failed_local_cubes);

    // Insert new cubes at the start of the local cubes
    _local_cubes.insert(_local_cubes.begin(), cubes.begin(), cubes.end());

    _request_state = NO_REQUEST;

    _local_cubes_cv.notify();
}