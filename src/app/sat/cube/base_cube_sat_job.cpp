#include <assert.h>

#include "util/console.hpp"
#include "cube_communicator.hpp"

#include "base_cube_sat_job.hpp"

BaseCubeSatJob::BaseCubeSatJob(Parameters& params, int commSize, int worldRank, int jobId) : 
        BaseSatJob(params, commSize, worldRank, jobId), _job_comm_period(params.getFloatParam("s")) {
}

bool BaseCubeSatJob::appl_initialize() {
    _lib = std::make_unique<CubeLib>(*(getDescription().getPayloads().at(0)));

    if (isRoot()) {
        _lib->generateCubes();
    }

    _cube_comm = (void*) new CubeCommunicator(this);

    return true;
}

bool BaseCubeSatJob::appl_doneInitializing() {
    return true;
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
    Console::log(Console::INFO, "Wants to Begin Comm?");
    if (isRoot()) {
        return true;
    }
    return !_lib->hasCubes();
}

void BaseCubeSatJob::appl_beginCommunication() {
    if (_cube_comm != NULL && !isRoot()) {
        Console::log(Console::INFO, "Sending request");
        ((CubeCommunicator*) _cube_comm)->requestCubes();
    }
    Console::log(Console::INFO, "Cube Comm is null");
}

void BaseCubeSatJob::appl_communicate(int source, JobMessage& msg) {
    Console::log(Console::INFO, "Communicating");
        if (_cube_comm != NULL) {
        Console::log(Console::INFO, "Sending request");
        ((CubeCommunicator*) _cube_comm)->handle(source, msg);
    }
    Console::log(Console::INFO, "Cube Comm is null");
}

bool BaseCubeSatJob::isInitialized() {
    assert(Console::fail("Not implemented yet!"));
}
void BaseCubeSatJob::prepareSharing(int maxSize) {
    assert(Console::fail("Not implemented yet!"));
}
bool BaseCubeSatJob::hasPreparedSharing() {
    assert(Console::fail("Not implemented yet!"));
}
std::vector<int> BaseCubeSatJob::getPreparedClauses() {
    assert(Console::fail("Not implemented yet!"));
}
void BaseCubeSatJob::digestSharing(const std::vector<int>& clauses) {
    assert(Console::fail("Not implemented yet!"));
}

void BaseCubeSatJob::prepareCubes() {
    //NOOP
}

bool BaseCubeSatJob::hasPreparedCubes() {
    return _lib->hasCubes();
}

std::vector<int> BaseCubeSatJob::getPreparedCubes() {
    return _lib->getPreparedCubes();
}

void BaseCubeSatJob::digestCubes(const std::vector<int>& cubes) {
    return _lib->digestCubes(cubes);
}

BaseCubeSatJob::~BaseCubeSatJob() {
    assert(Console::fail("Not implemented yet!"));
}