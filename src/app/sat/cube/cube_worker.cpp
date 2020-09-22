#include "cube_worker.hpp"

#include <cassert>

#include "app/sat/hordesat/solvers/cadical.hpp"
#include "app/sat/hordesat/solvers/cadical_interface.hpp"
#include "app/sat/hordesat/utilities/default_logging_interface.hpp"
#include "cube_communicator.hpp"
#include "util/console.hpp"

CubeWorker::CubeWorker(std::vector<int> &formula, CubeCommunicator &cube_comm, SatResult &result)
    : _formula(formula), _cube_comm(cube_comm), _result(result) {
    // Initialize logger
    _logger = std::unique_ptr<LoggingInterface>(
        new DefaultLoggingInterface(1, "<c-1>"));
    // Initialize solver
    _solver = std::unique_ptr<PortfolioSolverInterface>(
        new Cadical(*_logger, 1, 1, "c"));

    // Read formula
    for (int lit : _formula) {
        _solver->addLiteral(lit);
    }
}

void CubeWorker::mainLoop() {
    auto lock = _state_mutex.getLock();

    assert(_worker_state == IDLING);

    _worker_state = WAITING;

    // TODO: Check if this works. Otherwise extend CondVar.wait to take a unique lock or use std::mutex

    while (true) {
        // After the condition is fulfilled, the lock is reaquired
        _state_cond.wait(lock, [&] { return _worker_state == WORKING || _isInterrupted; });

        // Exit main loop
        if (_isInterrupted) {
            return;
        }

        // There should be local cubes available now
        assert(!_local_cubes.empty());

        // Start solving the local cubes
        SatResult result = solve();

        if (result == SAT) {
            _worker_state = SOLVED;
            _result = SAT;

        } else if (result == UNSAT){
            _worker_state = FAILED;
        }
    }
}

SatResult CubeWorker::solve() {
    _logger->log(1, "Solving.\n");

    for (Cube &next_local_cube : _local_cubes) {
        auto result = _solver->solve(next_local_cube.getPath());

        // Check result
        if (result == SAT) {
            _logger->log(1, "Found a solution.\n");
            return SAT;

        } else if (result == UNKNOWN) {
            _logger->log(1, "Solver interrupted.\n");
            return UNKNOWN;

        } else if (result == UNSAT) {
            _logger->log(1, "Cube failed.\n");
            next_local_cube.fail();
        }
    }
    // All cubes were unsatisfiable
    return UNSAT;
}

void CubeWorker::startWorking() {
    _worker_thread = std::thread(&CubeWorker::mainLoop, this);
}


void CubeWorker::interrupt() {
    _isInterrupted.store(true);
    // Exit solve if currently solving
    _solver->interrupt();
    // Resume worker thread if currently waiting
    _state_cond.notify();
}

void CubeWorker::join() {
    _worker_thread.join();
}

bool CubeWorker::wantsToCommunicate() {
    // Assures method does not return true twice on the same condition
    // TryLock prevents blocking
    if (!_state_mutex.tryLock()) {
        return false;
    }

    // The atomicity of the state assures that the state change is seen by all after the return
    if (_worker_state == WAITING) {
        // Worker was waiting for cubes
        _worker_state = REQUESTING;
        // Worker now waits to receive cubes

        _state_mutex.unlock();
        return true;

    } else if (_worker_state == FAILED) {
        // Worker was waiting to send his failed cubes
        _worker_state = RETURNING;
        // Worker now waits for his failed cubes to be received

        _state_mutex.unlock();
        return true;

    } else {
        _state_mutex.unlock();
        return false;
    }
}

void CubeWorker::beginCommunication() {
    if (_worker_state == REQUESTING) {
        _cube_comm.requestCubes();

    } else if (_worker_state == RETURNING) {
        auto serialized_failed_cubes = serializeCubes(_local_cubes);
        _cube_comm.returnFailedCubes(serialized_failed_cubes);

    } else {
        // TODO: Throw error
    }
}

void CubeWorker::handleMessage(int source, JobMessage &msg) {
    if (msg.tag == MSG_SEND_CUBES) {
        auto serialized_cubes = msg.payload;
        auto cubes = unserializeCubes(serialized_cubes);
        digestSendCubes(cubes);

    } else if (msg.tag == MSG_RECEIVED_FAILED_CUBES) {
        digestReveicedFailedCubes();

    } else {
        // TODO: Throw error
    }
}

void CubeWorker::digestSendCubes(std::vector<Cube> cubes) {
    const std::lock_guard<Mutex> lock(_state_mutex);
    assert(_worker_state == REQUESTING);

    _logger->log(1, "Digesting send cubes.\n");

    _local_cubes = cubes;

    // Cubes were digested
    // Worker can now work
    _worker_state = WORKING;
    _state_cond.notify();
}

void CubeWorker::digestReveicedFailedCubes() {
    const std::lock_guard<Mutex> lock(_state_mutex);
    assert(_worker_state == RETURNING);

    _logger->log(1, "Digesting received failed cubes.\n");

    // Failed cubes were returned
    // Worker can now request new cubes
    _worker_state = WAITING;
}
