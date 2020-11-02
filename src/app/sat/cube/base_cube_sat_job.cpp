#include "base_cube_sat_job.hpp"

#include <assert.h>

#include "util/console.hpp"

BaseCubeSatJob::BaseCubeSatJob(Parameters& params, int commSize, int worldRank, int jobId)
    : Job(params, commSize, worldRank, jobId),
      _logger(getIdentifier(), getLogfileSuffix()),
      _cube_comm(*this, _logger) {
}

bool BaseCubeSatJob::appl_initialize() {
    // Aquiring initialization mutex
    {
        const std::lock_guard<Mutex> lock(_initialization_mutex);

        // Check if job was aborted before initialization
        if (_isInterrupted) {
            // Lib was never initialized thus making the job destructable
            _logger.log(0, "%s : job was interrupted before initialization ", toStr());
            _job_state.store(State::DESTRUCTABLE);
            return false;
        }

        _logger.log(0, "%s : started intializing cube lib ", toStr());

        _job_state.store(INITIALIZING);

        CubeSetup cube_setup(getDescription().getPayloads().at(0), _cube_comm, _logger, _params, _sat_result);

        if (!isRoot()) {
            // Initialize cube lib with worker
            _lib = std::make_unique<CubeLib>(cube_setup);

            _logger.log(0, "%s : finished intializing cube lib with worker ", toStr());

            _job_state.store(ACTIVE);

            // If job was suspended before initialization. Respecting INITIALIZING_TO_SUSPENDED
            if (_isSuspended) {
                _lib->suspend();
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
    _logger.log(0, "%s : started generating cubes ", toStr());
    auto shouldStartWorkking = _lib->generateCubes();
    _logger.log(0, "%s : finished generating cubes ", toStr());

    // Aquiring initialization mutex
    {
        const std::lock_guard<Mutex> lock(_initialization_mutex);

        // Only turn active when there are cubes and the job was not interrupted
        if (shouldStartWorkking && !_isInterrupted) {
            _logger.log(0, "%s : finished intializing cube lib with root and worker ", toStr());

            _job_state.store(ACTIVE);

            // If job was suspended before initialization. Respecting INITIALIZING_TO_SUSPENDED
            if (_isSuspended) {
                _lib->suspend();
            }

            _lib->startWorking();

            return true;

        } else {
            // Initialization was aborted either because the formula was solved during cube generation or because of an interrupt during cube generation
            _logger.log(0, "%s : initialization was aborted ", toStr());
            _job_state.store(State::DESTRUCTABLE);
            _lib.reset();
            // TODO: Should we return here true or false
            return true;
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

    _logger.log(0, "%s : appl_pause was called", toStr());

    _isSuspended.store(true);

    // Job may only be suspended during
    if (_job_state == State::ACTIVE) {
        _lib->suspend();
    }
}

void BaseCubeSatJob::appl_unpause() {
    const std::lock_guard<Mutex> lock(_initialization_mutex);

    _logger.log(0, "%s : appl_unpause was called", toStr());

    _isSuspended.store(false);

    if (_job_state == State::ACTIVE) {
        _lib->resume();
    }
}

void BaseCubeSatJob::appl_interrupt() {
    const std::lock_guard<Mutex> lock(_initialization_mutex);

    _logger.log(0, "%s : appl_interrupt was called", toStr());

    interrupt_and_start_withdrawing();
}

void BaseCubeSatJob::appl_withdraw() {
    const std::lock_guard<Mutex> lock(_initialization_mutex);

    _logger.log(0, "%s : appl_withdraw was called", toStr());

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
            _isSuspended.store(false);
        }

        _job_state.store(State::WITHDRAWING);

        _withdraw_thread = std::thread(&BaseCubeSatJob::withdraw, this);
    }
}

void BaseCubeSatJob::withdraw() {
    _logger.log(0, "%s : started cleanup thread ", toStr());

    // Wait until worker is joined
    _lib->withdraw();

    const std::lock_guard<Mutex> lock(_initialization_mutex);

    _job_state.store(State::DESTRUCTABLE);

    _lib.reset();

    _logger.log(0, "%s : finished cleanup thread ", toStr());
}

int BaseCubeSatJob::appl_solveLoop() {
    if (_job_state != State::UNINITIALIZED && _job_state != State::INITIALIZING) {
        if (_sat_result != UNKNOWN) {
            _logger.log(0, "%s : found result %s", toStr(), _sat_result == 10 ? "SAT" : _sat_result == 20 ? "UNSAT" : "UNKNOWN");

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

    _logger.log(0, "%s : enter destructor ", toStr());

    // The withdraw thread might still be default constructed, because of an aborted initialization
    if (_withdraw_thread.joinable()) {
        _withdraw_thread.join();
        _logger.log(0, "%s : joined cleanup thread ", toStr());
    }

    _logger.log(0, "%s : exit destructor ", toStr());
}
