#include "dynamic_cube_lib.hpp"

#include <algorithm>
#include <cassert>
#include <iterator>

#include "app/sat/console_horde_interface.hpp"
#include "app/sat/hordesat/solvers/cadical.hpp"
#include "app/sat/hordesat/solvers/cadical_interface.hpp"
#include "cube_communicator.hpp"
#include "util/console.hpp"

DynamicCubeLib::DynamicCubeLib(DynamicCubeSetup &setup, bool isRoot) : _logger(setup.logger) {
    _solver_thread_count = 2;
    _max_dynamic_cubes = _solver_thread_count * 3;
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

    // Insert an empty cube into the dynamic cubes of the root lib instance
    if (isRoot) {
        Cube emptyCube;
        _dynamic_cubes.insert(emptyCube);
    }
}

void DynamicCubeLib::start() {
    const std::lock_guard<Mutex> lock(_local_lock);

    assert(_state.load() == INACTIVE);

    _logger.log(0, "Starting the dynamic cube lib");

    // Start all threads, they are going to request cubes by themselves
    for (auto &solver_thread : _solver_threads) solver_thread->start();
    for (auto &generator_thread : _generator_threads) generator_thread->start();

    _state.store(ACTIVE);
}

void DynamicCubeLib::interrupt() {
    const std::lock_guard<Mutex> lock(_local_lock);

    assert(_state.load() == ACTIVE);

    _logger.log(0, "Interrupting the dynamic cube lib");

    // Interrupt all threads, they will stop after some time
    for (auto &solver_thread : _solver_threads) solver_thread->interrupt();
    for (auto &generator_thread : _generator_threads) generator_thread->interrupt();

    _state.store(INTERRUPTING);
}

void DynamicCubeLib::join() {
    // No locking allowed, the threads need to succesfully call shareCubes or shareCubeToSplit

    assert(_state.load() == INTERRUPTING);

    _logger.log(0, "Joining the dynamic cube lib");

    // Joining all threads
    for (auto &solver_thread : _solver_threads) solver_thread->join();
    for (auto &generator_thread : _generator_threads) generator_thread->join();

    _state.store(INACTIVE);
}

void DynamicCubeLib::suspend() {
    _logger.log(0, "Suspending the dynamic cube lib");

    // First the lib is interrupted
    interrupt();

    // Secondly the lib is joined
    join();
    // -> All done work is persisted

    // Unassign all dynamic cubes to free them after
    _dynamic_cubes.resetAssignment();
};

void DynamicCubeLib::shareCubes(std::optional<Cube> &failedAssumptions, std::optional<Cube> &nextCube) {
    _logger.log(0, "shareCubes is called");

    auto lock = _local_lock.getLock();

    // Next cube must be empty at the beginning
    assert(!nextCube.has_value());

    if (failedAssumptions.has_value()) {
        handleFailedAssumptions(failedAssumptions.value());
    }

    while (true) {
        _logger.log(0, "CubeSolverThread entered cube retrieval loop");

        // Lib cannot be inactive
        assert(_state.load() != INACTIVE);

        // Notify generators
        // TODO Specify this
        _generator_cv.notify();

        // Leave cube empty on interruption
        if (_state.load() == INTERRUPTING) {
            _logger.log(0, "Lib was interrupted, no cube is shared");
            return;
        }

        // Try to get a cube
        nextCube = _dynamic_cubes.tryToGetACubeForSolving();

        if (nextCube.has_value()) {
            _logger.log(0, "CubeSolverThread retrieved a cube");
        } else {
            _logger.log(0, "No cube can be assigned, CubeSolverThread waits");
            // Wait because there are no solvable cubes
            _solver_cv.wait(lock, [&] { return _dynamic_cubes.hasACubeForSolving() || _state.load() == INTERRUPTING; });
        }
    }
}

void DynamicCubeLib::shareCubeToSplit(std::optional<Cube> &lastCube, int splitLit, std::optional<Cube> &failedAssumptions, std::optional<Cube> &nextCube) {
    _logger.log(0, "shareCubeToSplit is called");

    auto lock = _local_lock.getLock();

    // Next cube must be empty at the beginning
    assert(!nextCube.has_value());

    if (lastCube.has_value()) {
        if (splitLit != 0) {
            // If last cube is valid and was succesfully split handle split result
            bool addedNewCube = _dynamic_cubes.handleSplit(lastCube.value(), splitLit);

            if (addedNewCube) {
                // Dynamic cubes was succesfully extended
                // Notify starved solver and generator threads
                _solver_cv.notify();
                _generator_cv.notify();
            }
        } else {
            // If last cube is valid and was proven to be failing handle the failed assumptions
            assert(failedAssumptions.has_value());
            handleFailedAssumptions(failedAssumptions.value());
        }
    }

    while (true) {
        _logger.log(0, "CubeGeneratorThread entered cube retrieval loop");

        // Lib cannot be inactive
        assert(_state.load() != INACTIVE);

        if (_state.load() == INTERRUPTING) {
            _logger.log(0, "Lib was interrupted, no cube is shared");
            // Leave next cube empty on interruption
            return;

        } else if (_dynamic_cubes.size() > _max_dynamic_cubes) {
            _logger.log(0, "Too many cubes, CubeGeneratorThread waits");
            // Wait because there are too many cubes
            _generator_cv.wait(lock, [&] { return _dynamic_cubes.size() <= _solver_thread_count || _state.load() == INTERRUPTING; });

        } else if (!_dynamic_cubes.hasACubeForSplitting()) {
            if (_request_state == NONE && !_dynamic_cubes.hasSplittingCubes()) {
                _logger.log(0, "No cubes to split and no cube is splitting, set requesting");
                // Request new cubes because no cube can be split and no cube is being split
                _request_state = REQUESTING;
            }

            _logger.log(0, "No cube can be assigned, CubeGeneratorThread waits");
            // Wait because there are no splittable cubes
            // Because the thread is waiting for a new cube for splitting this does not wake and and set the state to requesting after getting notified
            _generator_cv.wait(lock, [&] { return _dynamic_cubes.hasACubeForSplitting() || _state.load() == INTERRUPTING; });

        } else {
            nextCube = _dynamic_cubes.tryToGetACubeForSplitting();

            assert(nextCube.has_value());

            _logger.log(0, "CubeGeneratorThread retrieved a cube");

            return;
        }
    }
}

void DynamicCubeLib::handleFailedAssumptions(Cube &failed) {
    // local lock must be held

    _logger.log(0, "Local thread found a failed assumption");

    auto path = failed.getPath();
    assert(!path.empty());

    // Remove all cubes containing the found failing assumption
    _dynamic_cubes.prune(failed);

    // Transform received failed assumptions from solver or generator to clause and add to local buffer
    for (int lit : path) _local_failed.push_back(-lit);

    // Append delimiter to buffer
    _local_failed.push_back(0);
}

bool DynamicCubeLib::isRequesting() {
    const std::lock_guard<Mutex> lock(_local_lock);

    assert(_state.load() == ACTIVE);

    if (_request_state == REQUESTING) {
        _request_state = RECEIVING;
        return true;
    } else {
        return false;
    }
}

std::vector<Cube> DynamicCubeLib::getCubes(size_t bias) {
    const std::lock_guard<Mutex> lock(_local_lock);

    assert(_state.load() == ACTIVE);

    size_t cubesToGet = std::max(_dynamic_cubes.size() - _solver_thread_count * 2, bias);

    return _dynamic_cubes.getFreeCubesForSending(cubesToGet);
}

void DynamicCubeLib::digestCubes(std::vector<Cube> &received_cubes) {
    const std::lock_guard<Mutex> lock(_local_lock);

    assert(_state.load() == ACTIVE);
    assert(!received_cubes.empty());

    _logger.log(0, "Digesting %zu cubes", received_cubes.size());

    // Reset request state
    _request_state = NONE;

    // Insert new cubes at the end of the local cubes
    _dynamic_cubes.insert(received_cubes);

    // Notify starved solver and generator threads
    _solver_cv.notify();
    _generator_cv.notify();
}

std::vector<Cube> DynamicCubeLib::releaseAllCubes() {
    const std::lock_guard<Mutex> lock(_local_lock);

    assert(_state.load() == INACTIVE);

    size_t localCubeCount = _dynamic_cubes.size();

    // Get all cubes
    std::vector<Cube> cubes = _dynamic_cubes.getFreeCubesForSending(localCubeCount);

    assert(cubes.size() == localCubeCount);
    assert(_dynamic_cubes.size() == 0);

    _logger.log(0, "Releasing %zu cubes", cubes.size());

    return cubes;
}

std::vector<int> DynamicCubeLib::getNewFailedAssumptions() {
    const std::lock_guard<Mutex> lock(_local_lock);

    // Can be called while the lib is active or after a suspension
    assert(_state.load() == INACTIVE || _state.load() == ACTIVE);

    auto failedAssumptions = _local_failed;
    _local_failed.clear();

    return failedAssumptions;
};

void DynamicCubeLib::digestFailedAssumptions(std::vector<int> &failed_assumptions) {
    // May be called all the time, the failed assumptions are buffered in the threads and are used when possible

    // Send failed assumptions to all threads
    for (auto &solver_thread : _solver_threads) solver_thread->handleFailed(failed_assumptions);
    for (auto &generator_thread : _generator_threads) generator_thread->handleFailed(failed_assumptions);
};