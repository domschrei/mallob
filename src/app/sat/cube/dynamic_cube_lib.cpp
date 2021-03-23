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
    _solver_thread_count = setup.params.getIntParam("t");
    _max_dynamic_cubes = _solver_thread_count * 2;
    _generator_thread_count = 1;

    // Create cube solver threads
    for (int i = 0; i < _solver_thread_count; i++) {
        _initializer_threads.emplace_back([this, setup]() {
            // https://stackoverflow.com/questions/3283778/why-can-i-not-push-back-a-unique-ptr-into-a-vector
            std::unique_ptr<DynamicCubeSolverThread> solver = std::make_unique<DynamicCubeSolverThread>(*this, setup);
            const std::lock_guard<Mutex> lock(_local_lock);
            _solver_threads.push_back(std::move(solver));
        });
    }

    // Create cube generator threads
    for (int i = 0; i < _generator_thread_count; i++) {
        _initializer_threads.emplace_back([this, setup]() {
            // https://stackoverflow.com/questions/3283778/why-can-i-not-push-back-a-unique-ptr-into-a-vector
            std::unique_ptr<DynamicCubeGeneratorThread> generator = std::make_unique<DynamicCubeGeneratorThread>(*this, setup);
            const std::lock_guard<Mutex> lock(_local_lock);
            _generator_threads.push_back(std::move(generator));
        });
    }

    _logger.log(0, "DynamicCubeLib: Started all initializer threads");

    for (auto &thread : _initializer_threads) thread.join();

    _logger.log(0, "DynamicCubeLib: Joined all initializer threads");

    // Insert an empty cube into the dynamic cubes of the root lib instance
    if (isRoot) {
        Cube emptyCube;
        _dynamic_cubes.insert(emptyCube);
    }
}

void DynamicCubeLib::start() {
    const std::lock_guard<Mutex> lock(_local_lock);

    assert(_state.load() == INACTIVE);

    _logger.log(0, "DynamicCubeLib: Starting the dynamic cube lib");

    // Start all threads, they are going to request cubes by themselves
    for (auto &solver_thread : _solver_threads) solver_thread->start();
    for (auto &generator_thread : _generator_threads) generator_thread->start();

    _state.store(ACTIVE);
}

void DynamicCubeLib::interrupt() {
    const std::lock_guard<Mutex> lock(_local_lock);

    assert(_state.load() == ACTIVE);

    _logger.log(0, "DynamicCubeLib: Interrupting the dynamic cube lib");

    // Notify all threads
    _solver_cv.notify();
    _generator_cv.notify();

    // Interrupt all threads, they will stop after some time
    for (auto &solver_thread : _solver_threads) solver_thread->interrupt();
    for (auto &generator_thread : _generator_threads) generator_thread->interrupt();

    _state.store(INTERRUPTING);
}

void DynamicCubeLib::join() {
    // No locking allowed, the threads need to succesfully call shareCubes or shareCubeToSplit

    assert(_state.load() == INTERRUPTING);

    _logger.log(0, "DynamicCubeLib: Joining the dynamic cube lib");

    // Joining all threads
    for (auto &solver_thread : _solver_threads) solver_thread->join();
    for (auto &generator_thread : _generator_threads) generator_thread->join();

    _state.store(INACTIVE);
}

void DynamicCubeLib::suspend() {
    _logger.log(0, "DynamicCubeLib: Suspending the dynamic cube lib");

    // First the lib is interrupted
    interrupt();

    // Secondly the lib is joined
    join();
    // -> All done work is persisted

    // Unassign all dynamic cubes to free them after
    _dynamic_cubes.resetAssignment();
}

void DynamicCubeLib::shareCube(std::optional<Cube> &failedAssumptions, std::optional<Cube> &nextCube, int id) {
    auto lock = _local_lock.getLock();

    _logger.log(0, "DynamicCubeSolverThread %i: entered shareCubes, %s", id, _dynamic_cubes.toString().c_str());

    // Next cube must be empty at the beginning
    assert(!nextCube.has_value());

    if (failedAssumptions.has_value()) {
        _logger.log(0, "DynamicCubeSolverThread %i: added new failed assumptions", id);

        handleFailedAssumptions(failedAssumptions.value());
    }

    while (true) {
        _logger.log(0, "DynamicCubeSolverThread %i: entered cube retrieval loop", id);

        // Lib cannot be inactive
        assert(_state.load() != INACTIVE);

        // Notify generators
        // TODO Specify this
        _generator_cv.notify();

        // Leave cube empty on interruption
        if (_state.load() == INTERRUPTING) {
            _logger.log(0, "DynamicCubeSolverThread %i: did not get a cube because the lib is interrupted", id);
            return;
        }

        // Try to get a cube
        nextCube = _dynamic_cubes.tryToGetACubeForSolving();

        if (nextCube.has_value()) {
            _logger.log(0, "DynamicCubeSolverThread %i: retrieved a cube", id);
            return;

        } else {
            _logger.log(0, "DynamicCubeSolverThread %i: waits because no cube could be assigned", id);

            // Wait because there are no solvable cubes
            _solver_cv.wait(lock, [this, id] {
                _logger.log(0, "DynamicCubeSolverThread %i: was notified, %s", id, _dynamic_cubes.toString().c_str());
                return _dynamic_cubes.hasACubeForSolving() || _state.load() == INTERRUPTING;
            });

            _logger.log(0, "DynamicCubeSolverThread %i: resumes because a cube could be assigned", id);
        }
    }
}

void DynamicCubeLib::shareCubeToSplit(std::optional<Cube> &lastCube, int splitLit, std::optional<Cube> &failedAssumptions, std::optional<Cube> &nextCube,
                                      int id) {
    auto lock = _local_lock.getLock();

    _logger.log(0, "DynamicCubeGeneratorThread %i: entered shareCubeToSplit, %s", id, _dynamic_cubes.toString().c_str());

    // Next cube must be empty at the beginning
    assert(!nextCube.has_value());

    if (lastCube.has_value()) {
        if (splitLit != 0) {
            // If last cube is valid and was succesfully split handle split result
            bool addedNewCube = _dynamic_cubes.handleSplit(lastCube.value(), splitLit);

            if (addedNewCube) {
                _logger.log(0, "DynamicCubeGeneratorThread %i: created a new dynamic cube with size %zu", id, lastCube.value().getPath().size() + 1);

                // Dynamic cubes was succesfully extended
                // Notify starved solver and generator threads
                _solver_cv.notify();
                _generator_cv.notify();

            } else {
                _logger.log(0, "DynamicCubeGeneratorThread %i: could not create a new dynamic cube, the expanded cube was pruned", id);
            }

        } else {
            _logger.log(0, "DynamicCubeGeneratorThread %i: added new failed assumptions", id);

            // If last cube is valid and was proven to be failing handle the failed assumptions
            assert(failedAssumptions.has_value());
            handleFailedAssumptions(failedAssumptions.value());
        }
    }

    while (true) {
        _logger.log(0, "DynamicCubeGeneratorThread %i: entered cube retrieval loop", id);

        // Lib cannot be inactive
        assert(_state.load() != INACTIVE);

        if (_state.load() == INTERRUPTING) {
            _logger.log(0, "DynamicCubeGeneratorThread %i: did not get a cube because the lib is interrupted", id);
            // Leave next cube empty on interruption
            return;

        } else if (static_cast<int>(_dynamic_cubes.size()) >= _max_dynamic_cubes) {
            _logger.log(0, "DynamicCubeGeneratorThread %i: waits because there are too many cubes", id);

            // Increment waiting generators
            _waiting_generator_threads++;

            // Wait because there are too many cubes
            _generator_cv.wait(lock, [this, id] {
                _logger.log(0, "DynamicCubeGeneratorThread %i: was notified, %s", id, _dynamic_cubes.toString().c_str());
                return static_cast<int>(_dynamic_cubes.size()) < _max_dynamic_cubes || _state.load() == INTERRUPTING;
            });

            // Decrement waiting generators
            _waiting_generator_threads--;

            _logger.log(0, "DynamicCubeGeneratorThread %i: resumes because there are no longer too many cubes", id);

        } else if (!_dynamic_cubes.hasACubeForSplitting()) {
            if (_request_state == NONE && !_dynamic_cubes.hasSplittingCubes()) {
                _logger.log(0, "There are no cubes to split and no cube is splitting, set requesting");
                // Request new cubes because no cube can be split and no cube is being split
                _request_state = REQUESTING;
            }
            _logger.log(0, "DynamicCubeGeneratorThread %i: waits because no cube could be assigned", id);

            // Wait because there are no splittable cubes
            // Because the thread is waiting for a new cube for splitting this does not wake and and set the state to requesting after getting notified
            _generator_cv.wait(lock, [this, id] {
                _logger.log(0, "DynamicCubeGeneratorThread %i: was notified, %s", id, _dynamic_cubes.toString().c_str());
                // Resume when
                // 1. There is a cube to split
                // 2. There are no cubes to split and the lib is not requesting and no cube is being split
                // TODO Problem with multiple generators (If a cube that is being split is pruned the program does not know that a generator is still running)
                // -> But this should not cause problems. The lib should start to request. A generator thread could just unecessarly resume and then wait.
                // 3. The lib was interrupted
                return _dynamic_cubes.hasACubeForSplitting() || (_request_state == NONE && !_dynamic_cubes.hasSplittingCubes()) ||
                       _state.load() == INTERRUPTING;
            });

            _logger.log(0, "DynamicCubeGeneratorThread %i: resumes because a cube could be assigned", id);

        } else {
            nextCube = _dynamic_cubes.tryToGetACubeForSplitting();

            assert(nextCube.has_value());

            _logger.log(0, "DynamicCubeGeneratorThread %i: retrieved a cube", id);

            return;
        }
    }
}

void DynamicCubeLib::handleFailedAssumptions(Cube failed) {
    // local lock must be held

    _logger.log(0, "Before handleFailedAssumptions(), %s", _dynamic_cubes.toString().c_str());

    auto path = failed.getPath();
    assert(!path.empty());

    // Remove all cubes containing the found failing assumption
    _dynamic_cubes.prune(failed);

    // Transform received failed assumptions from solver or generator to clause and add to local buffer
    for (int lit : path) _local_failed.push_back(-lit);

    // Append delimiter to buffer
    _local_failed.push_back(0);

    _logger.log(0, "After handleFailedAssumptions(), %s", _dynamic_cubes.toString().c_str());
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

std::vector<Cube> DynamicCubeLib::getCubes(int bias) {
    const std::lock_guard<Mutex> lock(_local_lock);

    assert(_state.load() == ACTIVE);

    _logger.log(0, "Before getCubes(), %s", _dynamic_cubes.toString().c_str());

    // Only so many cubes may be shared, that every solver thread can still get one
    int number_shareable_cubes = static_cast<int>(_dynamic_cubes.size()) - _solver_thread_count;

    // Using initializer list for comparing 3 values
    // https://codereview.stackexchange.com/questions/26100/maximum-of-three-values-in-c
    int cubesToGet = std::min(std::max(bias, 0), number_shareable_cubes);

    // Notify generator
    _generator_cv.notify();

    auto freeCubes = _dynamic_cubes.getFreeCubesForSending(cubesToGet);

    _logger.log(0, "After getCubes(), %s", _dynamic_cubes.toString().c_str());

    return freeCubes;
}

void DynamicCubeLib::digestCubes(std::vector<Cube> &received_cubes) {
    const std::lock_guard<Mutex> lock(_local_lock);

    assert(_state.load() == ACTIVE);
    assert(!received_cubes.empty());

    _logger.log(0, "Digesting %zu cubes", received_cubes.size());

    _logger.log(0, "Before digestCubes(), %s", _dynamic_cubes.toString().c_str());

    // Reset request state
    _request_state = NONE;

    // Insert new cubes at the end of the local cubes
    _dynamic_cubes.insert(received_cubes);

    _logger.log(0, "After digestCubes(), %s", _dynamic_cubes.toString().c_str());

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
}

void DynamicCubeLib::digestFailedAssumptions(std::vector<int> &failed_assumptions) {
    // May be called all the time, the failed assumptions are buffered in the threads and are used when possible

    // Send failed assumptions to all threads
    for (auto &solver_thread : _solver_threads) solver_thread->handleFailed(failed_assumptions);
    for (auto &generator_thread : _generator_threads) generator_thread->handleFailed(failed_assumptions);
}

bool DynamicCubeLib::allCubesGenerated() {
    return _generator_thread_count == _waiting_generator_threads;
}