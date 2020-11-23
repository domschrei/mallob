#include "base_cube_sat_job.hpp"

#include <assert.h>

#include "util/console.hpp"

// worldRank is mpi rank
// job id is id of job
// where is rank of job of this node -> it gets entered to _name before appl_initialize is called

BaseCubeSatJob::BaseCubeSatJob(Parameters& params, int commSize, int worldRank, int jobId)
    : Job(params, commSize, worldRank, jobId),
      _logger("<c-" + std::to_string(_world_rank) + std::string(toStr()) + ">", std::string(toStr())),
      _cube_comm(*this, _logger) {}

bool BaseCubeSatJob::appl_initialize() {
    // Aquiring initialization mutex
    {
        const std::lock_guard<Mutex> lock(_initialization_mutex);

        // Update _logger
        _logger.setIdentifier("<c-" + std::to_string(_world_rank) + std::string(toStr()) + ">");
        _logger.log(0, "Logger was updated");

        // Check if job was aborted before initialization
        if (_isInterrupted) {
            // Lib was never initialized thus making the job destructable
            _logger.log(0, "Job was interrupted before initialization");
            _job_state.store(State::DESTRUCTABLE);
            return false;
        }

        _logger.log(0, "Started intializing cube lib");

        _job_state.store(INITIALIZING);

        CubeSetup cube_setup(getDescription().getPayloads().at(0), _cube_comm, _logger, _params, _sat_result);

        if (!isRoot()) {
            // Initialize cube lib with worker
            _lib = std::make_unique<CubeLib>(cube_setup);

            _logger.log(0, "Finished intializing cube lib with worker");

            _job_state.store(ACTIVE);

            // If job was suspended before initialization. Respecting INITIALIZING_TO_SUSPENDED
            // Set _since accordingly
            if (_isSuspended) {
                _lib->suspend();
                _suspended_since = _logger.getTime();
            } else {
                _working_since = _logger.getTime();
            }

            _lib->startWorking();

            return true;

        } else {
            // Initialize cube lib with root and worker
            _lib = std::make_unique<CubeLib>(cube_setup);
        }
    }
    // Release initialization mutex

    // Generate cubes
    // This cannot be suspended but is interruptable
    _logger.log(0, "Started generating cubes");
    auto shouldStartWorkking = _lib->generateCubes();
    _logger.log(0, "Finished generating cubes");

    // Aquiring initialization mutex
    {
        const std::lock_guard<Mutex> lock(_initialization_mutex);

        // Only turn active when there are cubes and the job was not interrupted
        if (shouldStartWorkking && !_isInterrupted) {
            _logger.log(0, "Finished intializing cube lib with root and worker");

            _job_state.store(ACTIVE);

            // If job was suspended before initialization. Respecting INITIALIZING_TO_SUSPENDED
            // Set _since accordingly
            if (_isSuspended) {
                _lib->suspend();
                _suspended_since = _logger.getTime();
            } else {
                _working_since = _logger.getTime();
            }

            _lib->startWorking();

            return true;

        } else {
            // Initialization was aborted either because the formula was solved during cube generation or because of an interrupt during cube generation
            _logger.log(0, "Initialization was aborted");

            _job_state.store(State::DESTRUCTABLE);
            _lib.reset();

            // Return true when formula was solved during cube generation
            // Return false if interrupted during cube generation
            return _sat_result != UNKNOWN ? true : false;
        }
    }
    // Release initialization mutex
}

bool BaseCubeSatJob::appl_doneInitializing() {
    return _job_state != State::UNINITIALIZED && _job_state != State::INITIALIZING;
}

void BaseCubeSatJob::appl_updateRole() {
    assert(Console::fail("Not implemented yet!"));
}

void BaseCubeSatJob::appl_updateDescription(int fromRevision) {
    assert(Console::fail("Not implemented yet!"));
}

void BaseCubeSatJob::appl_pause() {
    const std::lock_guard<Mutex> lock(_initialization_mutex);

    _logger.log(0, "appl_pause was called");

    // Do nothing if already suspended
    if (_isSuspended) return;

    _isSuspended.store(true);

    if (_job_state == State::ACTIVE) {
        _lib->suspend();

        double pause_time = _logger.getTime();
        _working_duration = pause_time - _working_since;
        _suspended_since = pause_time;
    }
}

void BaseCubeSatJob::appl_unpause() {
    const std::lock_guard<Mutex> lock(_initialization_mutex);

    _logger.log(0, "appl_unpause was called");

    // Do nothing if already unsuspended
    if (!_isSuspended) return;

    _isSuspended.store(false);

    if (_job_state == State::ACTIVE) {
        _lib->resume();

        double unpause_time = _logger.getTime();
        _suspended_duration += unpause_time - _suspended_since;
        _working_since = unpause_time;
    }
}

void BaseCubeSatJob::appl_interrupt() {
    const std::lock_guard<Mutex> lock(_initialization_mutex);

    _logger.log(0, "appl_interrupt was called");

    interrupt_and_start_withdrawing();
}

void BaseCubeSatJob::appl_withdraw() {
    const std::lock_guard<Mutex> lock(_initialization_mutex);

    _logger.log(0, "appl_withdraw was called");

    interrupt_and_start_withdrawing();
}

void BaseCubeSatJob::interrupt_and_start_withdrawing() {
    _isInterrupted.store(true);

    if (_job_state == State::INITIALIZING) {
        _lib->interrupt();
    }

    if (_job_state == State::ACTIVE) {
        _lib->interrupt();

        // Resume worker thread if necessary to allow termination
        if (_isSuspended) {
            _lib->resume();
        }

        _job_state.store(State::WITHDRAWING);

        _withdraw_thread = std::thread(&BaseCubeSatJob::withdraw, this);
    }

    // Calculate duration of last segment
    if (_isSuspended) {
        _suspended_duration = _logger.getTime() - _suspended_since;
    } else {
        _working_duration = _logger.getTime() - _working_since;
    }
}

void BaseCubeSatJob::withdraw() {
    _logger.log(0, "Started withdraw thread");

    // Wait until worker is joined
    _lib->withdraw();

    const std::lock_guard<Mutex> lock(_initialization_mutex);

    _job_state.store(State::DESTRUCTABLE);

    _lib.reset();

    _logger.log(0, "Finished withdraw thread");
}

int BaseCubeSatJob::appl_solveLoop() {
    if (_job_state != State::UNINITIALIZED && _job_state != State::INITIALIZING) {
        if (_sat_result != UNKNOWN) {
            _logger.log(0, "Found result %s", _sat_result == 10 ? "SAT" : _sat_result == 20 ? "UNSAT" : "UNKNOWN");

            _result.id = getId();
            _result.result = _sat_result;
            _result.revision = getDescription().getRevision();
            _result.solution.clear();

            return 1;
        }
    }
    // Default case
    return -1;
}

void BaseCubeSatJob::appl_dumpStats() {}

bool BaseCubeSatJob::appl_isDestructible() {
    return _job_state == State::DESTRUCTABLE;
}

// Messages are only required during ACTIVE to guarantee correct solving.
// This allows all communication to be completed, regardless of suspension.
// Messages do not need to be answered during WITHDRAWING or DESTRUCTABLE. The worker automatically terminates after a call to interrupt.
// Locking would be required to prevent race conditions. This can be omitted because the job is only controlled by a single thread.
bool BaseCubeSatJob::appl_wantsToBeginCommunication() const {
    if (_job_state == State::ACTIVE)
        return _lib->wantsToCommunicate();
    else
        return false;
}

void BaseCubeSatJob::appl_beginCommunication() {
    if (_job_state == State::ACTIVE)
        _lib->beginCommunication();
}

void BaseCubeSatJob::appl_communicate(int source, JobMessage& msg) {
    if (_job_state == State::ACTIVE)
        _lib->handleMessage(source, msg);
}

int BaseCubeSatJob::getDemand(int prevVolume, float elapsedTime) const {
    if (_job_state != State::ACTIVE)
        return 1;
    else
        return Job::getDemand(prevVolume, elapsedTime);
}

BaseCubeSatJob::~BaseCubeSatJob() {
    const std::lock_guard<Mutex> lock(_initialization_mutex);

    _logger.log(0, "Enter destructor");

    // Print durations
    _logger.log(0, "Time working: %.3f", _working_duration);
    _logger.log(0, "Time suspended: %.3f", _suspended_duration);

    // The withdraw thread might still be default constructed, because of an aborted initialization
    if (_withdraw_thread.joinable()) {
        _withdraw_thread.join();
        _logger.log(0, "Joined cleanup thread");
    }

    _logger.log(0, "Exit destructor");
}
