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
    assert(Console::fail("Not implemented yet!"));
}

void BaseCubeSatJob::appl_withdraw() {
    assert(Console::fail("Not implemented yet!"));
}

int BaseCubeSatJob::appl_solveLoop() {
    return -1;
}

void BaseCubeSatJob::appl_dumpStats() {
}

bool BaseCubeSatJob::appl_isDestructible() {
    return false;
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

BaseCubeSatJob::~BaseCubeSatJob() {
    assert(Console::fail("Not implemented yet!"));
}