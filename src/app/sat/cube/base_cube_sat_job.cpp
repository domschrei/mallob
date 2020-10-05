#include "base_cube_sat_job.hpp"

#include <assert.h>

#include "../horde_config.hpp"
#include "util/console.hpp"

BaseCubeSatJob::BaseCubeSatJob(Parameters& params, int commSize, int worldRank, int jobId)
    : Job(params, commSize, worldRank, jobId),
      _logger(getIdentifier(), getLogfileSuffix()),
      _cube_comm(*this, _logger) {
}

bool BaseCubeSatJob::appl_initialize() {
    // Aquire initialization lock
    const std::lock_guard<Mutex> lock(_manipulation_mutex);

    // Check if job was aborted before initialization
    if (_abort_before_initialization) {
        // Lib was never initialized thus making the job destructable
        // This does not lead to a call of the destructor before this method returns
        // because a job is never deleted before it finishes its initialization
        _logger.log(0, "%s : abort intializing cube lib ", toStr());
        _isDestructible.store(true);
        return false;
    }

    // Get formula
    std::vector<int> formula = *(getDescription().getPayloads().at(0));

    _logger.log(0, "%s : started intializing cube lib ", toStr());

    Parameters hParams(_params);
    HordeConfig::applyDefault(hParams, *this);

    if (isRoot()) {
        // Initialize cube lib with root and worker
        _lib = std::make_unique<CubeLib>(hParams, formula, _cube_comm, _logger, 5, 4);

        // Generate cubes
        _logger.log(0, "%s : started generating cubes ", toStr());
        auto solved = _lib->generateCubes();
        _logger.log(0, "%s : finished generating cubes ", toStr());

        // Check if formula was solved durin cube generation
        if (solved) {
            _logger.log(0, "%s : solved formula during cube generation", toStr());

            // CubeLib was succesfully initialized
            _isInitialized.store(true);

            // Worker thread is never started
            _isDestructible.store(true);
            return true;
        }

    } else {
        // Initialize cube lib with worker
        _lib = std::make_unique<CubeLib>(hParams, formula, _cube_comm, _logger);
    }

    _logger.log(0, "%s : finished intializing cube lib ", toStr());

    // Succesfully initialized
    _isInitialized.store(true);

    // Set working flag
    _isWorking.store(true);

    // If job was suspended before initialization, respecting INITIALIZING_TO_SUSPENDED
    if (_isSuspended) {
        // Set suspension flag in _lib
        _lib->suspend();
    }
    
    // Start working
    _lib->startWorking();

    return true;
}

bool BaseCubeSatJob::appl_doneInitializing() {
    return _isInitialized;
}

void BaseCubeSatJob::appl_updateRole() {
    assert(Console::fail("Not implemented yet!"));
}

void BaseCubeSatJob::appl_updateDescription(int fromRevision) {
    assert(Console::fail("Not implemented yet!"));
}

void BaseCubeSatJob::appl_pause() {
    // Aquire initialization lock
    const std::lock_guard<Mutex> lock(_manipulation_mutex);

    _logger.log(0, "%s : appl_pause was called", toStr());

    _isSuspended = true;

    if (_isWorking && !_isWithdrawing) {
        _lib->suspend();
    }
}

// Critical
void BaseCubeSatJob::appl_unpause() {
    // Aquire initialization lock
    const std::lock_guard<Mutex> lock(_manipulation_mutex);

    _logger.log(0, "%s : appl_unpause was called", toStr());

    _isSuspended = false;

    // Job is also unsuspended during withdraw
    // These checks prevents calling resume twice
    if (_isWorking && _isSuspended && !_isWithdrawing) {
        _lib->resume();
    }
}

void BaseCubeSatJob::appl_interrupt() {
    // Aquire initialization lock
    const std::lock_guard<Mutex> lock(_manipulation_mutex);

    _logger.log(0, "%s : appl_interrupt was called", toStr());

    if (_isWorking) {
        // Uncritical
        _lib->interrupt();
    } else {
        // Set flag to abort subsequent initialization
        // Otherwise wait for preceding initialization to finish
        _abort_before_initialization.store(true);
    }
}

void BaseCubeSatJob::appl_withdraw() {
    // Aquire initialization lock
    const std::lock_guard<Mutex> lock(_manipulation_mutex);

    _logger.log(0, "%s : appl_withdraw was called", toStr());

    if (_isWorking) {
        // Uncritical
        _lib->interrupt();

        // Job may only be withdrawn once
        if (!_isWithdrawing) {
            _isWithdrawing = true;
            _withdraw_thread = std::thread(&BaseCubeSatJob::withdraw, this);
        }
    } else {
        // Set flag to abort subsequent initialization
        // Otherwise wait for preceding initialization to finish
        _abort_before_initialization.store(true);
    }
}

void BaseCubeSatJob::withdraw() {
    _logger.log(0, "%s : started cleanup thread ", toStr());

    // Resume worker thread if necessary to allow termination
    if (_isSuspended) {
        _lib->resume();
    }

    _lib->withdraw();
    _isDestructible.store(true);

    _logger.log(0, "%s : finished cleanup thread ", toStr());
}

int BaseCubeSatJob::appl_solveLoop() {
    if (_isInitialized) {
        SatResult result = _lib->getResult();

        if (result != UNKNOWN) {
            _logger.log(0, "%s : found result %s", toStr(), result == 10 ? "SAT" : result == 20 ? "UNSAT" : "UNKNOWN");

            _result.id = getId();
            _result.result = result;
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
    return _isDestructible.load();
}

// Noop in _lib when interrupted -> No _isWithdrawing check required
bool BaseCubeSatJob::appl_wantsToBeginCommunication() const {
    if (_isWorking)
        return _lib->wantsToCommunicate();
    else
        return false;
}

// Noop in _lib when interrupted -> No _isWithdrawing check required
void BaseCubeSatJob::appl_beginCommunication() {
    if (_isWorking) {
        _lib->beginCommunication();
    }
}

// Noop in _lib when interrupted -> No _isWithdrawing check required
void BaseCubeSatJob::appl_communicate(int source, JobMessage& msg) {
    if (_isWorking)
        _lib->handleMessage(source, msg);
}

int BaseCubeSatJob::getDemand(int prevVolume, float elapsedTime) const {
    if (!_isWorking)
        return 1;
    else
        return Job::getDemand(prevVolume, elapsedTime);
}

BaseCubeSatJob::~BaseCubeSatJob() {
    // Aquire initialization lock
    const std::lock_guard<Mutex> lock(_manipulation_mutex);

    _logger.log(0, "%s : enter destructor ", toStr());

    // The withdraw thread might still be default constructed, because of an aborted initialization
    if (_withdraw_thread.joinable()) {
        _withdraw_thread.join();
        _logger.log(0, "%s : joined cleanup thread ", toStr());
    }

    _logger.log(0, "%s : exit destructor ", toStr());
}
