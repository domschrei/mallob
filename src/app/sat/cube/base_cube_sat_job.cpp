#include "base_cube_sat_job.hpp"

#include <assert.h>

#include "util/console.hpp"

BaseCubeSatJob::BaseCubeSatJob(Parameters& params, int commSize, int worldRank, int jobId)
    : Job(params, commSize, worldRank, jobId), _cube_comm(this) {}

bool BaseCubeSatJob::appl_initialize() {
    // Get formula
    std::vector<int> formula = *(getDescription().getPayloads().at(0));

    if (isRoot()) {
        // Initialize cube lib with root and worker
        _lib = std::make_unique<CubeLib>(formula, _cube_comm, 5, 4);
        // Generate cubes
        _lib->generateCubes();
    } else {
        // Initialize cube lib with worker
        _lib = std::make_unique<CubeLib>(formula, _cube_comm);
    }

    _isInitialized.store(true);

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
    assert(Console::fail("Not implemented yet!"));
}

void BaseCubeSatJob::appl_unpause() {
    assert(Console::fail("Not implemented yet!"));
}

void BaseCubeSatJob::appl_interrupt() {
    if (_isInitialized) {
        _lib->interrupt();
    }
}

void BaseCubeSatJob::appl_withdraw() {
    if (_isInitialized) {
        // TODO: Make asynchronous
        _lib->withdraw();
        _isDestructible.store(true);
    }
}

int BaseCubeSatJob::appl_solveLoop() {
    if (_isInitialized) {
        SatResult result = _lib->getResult();

        if (result != UNKNOWN) {
            Console::log_send(Console::INFO, getRootNodeRank(), "%s : found result %s", toStr(),
                              result == 10 ? "SAT" : result == 20 ? "UNSAT" : "UNKNOWN");

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

void BaseCubeSatJob::appl_dumpStats() {
}

bool BaseCubeSatJob::appl_isDestructible() {
    return _isDestructible.load();
}

bool BaseCubeSatJob::appl_wantsToBeginCommunication() const {
    if (_isInitialized)
        return _lib->wantsToCommunicate();
    else
        return false;
}

void BaseCubeSatJob::appl_beginCommunication() {
    if (_isInitialized) {
        return _lib->beginCommunication();
    }
}

void BaseCubeSatJob::appl_communicate(int source, JobMessage& msg) {
    if (_isInitialized && this->isInState({JobState::ACTIVE}))
        return _lib->handleMessage(source, msg);
}

int BaseCubeSatJob::getDemand(int prevVolume, float elapsedTime) const {
    if (!_isInitialized)
        return 1;
    else
        return Job::getDemand(prevVolume, elapsedTime);
}
