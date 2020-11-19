#include "cube_worker_greedy.hpp"

#include <cassert>
#include <iterator>

#include "app/sat/console_horde_interface.hpp"
#include "app/sat/hordesat/solvers/cadical.hpp"
#include "app/sat/hordesat/solvers/cadical_interface.hpp"
#include "cube_communicator.hpp"
#include "util/console.hpp"

CubeWorkerGreedy::CubeWorkerGreedy(CubeSetup &setup) : CubeWorkerInterface(setup) {
    _thread_count = setup.params.getIntParam("t");
    _batch_size = setup.params.getIntParam("cubes-per-worker") / setup.params.getIntParam("t");

    for (size_t i = 0; i < _thread_count; i++) {
        // https://stackoverflow.com/questions/3283778/why-can-i-not-push-back-a-unique-ptr-into-a-vector
        std::unique_ptr<CubeSolverThread> solver = std::make_unique<CubeSolverThread>(this, setup);
        _solver_threads.push_back(std::move(solver));
    }
}

void CubeWorkerGreedy::startWorking() {
    for (auto &solver_thread : _solver_threads)
        solver_thread->start();
}

void CubeWorkerGreedy::shareCubes(std::vector<Cube> &cubes, std::vector<Cube> &failed) {
    _logger.log(0, "shareCubes is called", _local_cubes.size());

    auto lock = _local_cubes_mutex.getLock();

    // Vector must be empty on call
    assert(cubes.empty());

    // Handle failed cubes
    receiveFailed(failed);

    while (cubes.empty()) {
        _logger.log(0, "CubeSolverThread entered cube retrieval loop, currently there are %zu cubes", _local_cubes.size());

        // Share no cubes if interrupted
        if (_isInterrupted)
            return;

        // Set flag to request cubes if necessary
        if (_request_state == NO_REQUEST && _local_cubes.size() < _thread_count * _batch_size)
            _request_state = REQUESTING;

        // Try to get a cube
        if (_local_cubes.empty()) {
            // This should work
            // 1. No worker thread can start waiting when then communication
            // thread holds the _local_cubes_mutex
            // 2. After a notify_all all worker threads that were previously
            // waiting now compete for _local_cubes_mutex
            // -> It is good that all wake up since we received cubes that satsisfy multiple threads
            // 3. If a worker thread wakes up gains the lock and there are no cubes left it
            // starts to wait again
            _local_cubes_cv.wait(lock, [&] { return !_local_cubes.empty() || _isInterrupted; });

        } else {
            // Calculate how many cubes are taken
            // There is at least one cube because of the if clause
            size_t number_of_cubes = std::min(_local_cubes.size(), _batch_size);
            // https://stackoverflow.com/questions/15004517/moving-elements-from-stdvector-to-another-one
            // Use a make_move_iterator and the insert function
            // to move the #number_of_cubes last elements from _local_cubes to worker_thread_cubes
            auto start = std::make_move_iterator(_local_cubes.end() - number_of_cubes);
            auto end = std::make_move_iterator(_local_cubes.end());
            cubes.insert(cubes.end(), start, end);
            // Remove the now empty fields in _local_cubes
            // Do not use resize since Cube has no default constructor
            _local_cubes.erase(_local_cubes.end() - number_of_cubes, _local_cubes.end());

            _logger.log(0, "CubeSolverThread retrieved %zu cubes", number_of_cubes);
        }
    }
}

void CubeWorkerGreedy::shareFailed(std::vector<Cube> &failed) {
    auto lock = _local_cubes_mutex.getLock();

    receiveFailed(failed);
}

void CubeWorkerGreedy::receiveFailed(std::vector<Cube> &failed) {
    // _local_cubes_mutex must be held

    _failed_local_cubes.insert(_failed_local_cubes.end(), failed.begin(), failed.end());

    _logger.log(0, "Added %zu failed local cubes", failed.size());

    std::function<bool(Cube &)> includesPredicate = [&failed](Cube &cube) {
        for (Cube &failed_cube : failed)
            if (cube.includes(failed_cube))
                return true;

        return false;
    };

    auto sizeBefore = _local_cubes.size();

    // Erases all local cubes that include a failed cube
    // Behavior is defined if no local cube matches
    // https://stackoverflow.com/questions/24011627/erasing-using-iterator-from-find-or-remove
    // Function follows Erase-remove idiom https://en.wikipedia.org/wiki/Erase%E2%80%93remove_idiom
    _local_cubes.erase(std::remove_if(_local_cubes.begin(), _local_cubes.end(), includesPredicate), _local_cubes.end());

    auto sizeAfter = _local_cubes.size();

    _logger.log(0, "Pruned %zu local cubes", sizeBefore - sizeAfter);
}

void CubeWorkerGreedy::interrupt() {
    // Needed like in definition of conditional variables
    // https://en.cppreference.com/w/cpp/thread/condition_variable
    auto lock = _local_cubes_mutex.getLock();

    _isInterrupted.store(true);

    // Interrupt solver threads
    for (auto &solver_thread : _solver_threads)
        solver_thread->interrupt();

    // Resume solver threads that are currently waiting in shareCubes
    _local_cubes_cv.notify();
}

void CubeWorkerGreedy::join() {
    // Clearing _solver_threads this calls the destructor of each solver thread which joins the internal thread
    _solver_threads.clear();
}

void CubeWorkerGreedy::suspend() {
    for (auto &solver_thread : _solver_threads) solver_thread->suspend();
}

void CubeWorkerGreedy::resume() {
    for (auto &solver_thread : _solver_threads) solver_thread->unsuspend();
}

bool CubeWorkerGreedy::wantsToCommunicate() {
    return _request_state == REQUESTING;
}

void CubeWorkerGreedy::beginCommunication() {
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

    // Insert new cubes at the start of the local cubes
    _local_cubes.insert(_local_cubes.begin(), cubes.begin(), cubes.end());

    _request_state = NO_REQUEST;

    _local_cubes_cv.notify();
}