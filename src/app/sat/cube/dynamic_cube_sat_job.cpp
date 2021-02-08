#include "dynamic_cube_sat_job.hpp"

#include <cassert>

#include "dynamic_cube_communicator.hpp"
#include "new_dynamic_cube_communicator.hpp"
#include "failed_assumption_communicator.hpp"
#include "util/console.hpp"

// worldRank is mpi rank
// job id is id of job
// where is rank of job of this node -> it gets entered to _name before appl_initialize is called

DynamicCubeSatJob::DynamicCubeSatJob(Parameters& params, int commSize, int worldRank, int jobId)
    : Job(params, commSize, worldRank, jobId),
      _logger("<c-" + std::to_string(_world_rank) + std::string(toStr()) + ">", std::string(toStr())),
      _job_comm_period(params.getFloatParam("s")) {
    _dynamic_cube_comm = new NewDynamicCubeCommunicator(*this, _logger, params.getIntParam("t"));
    _failed_assumption_comm = new FailedAssumptionCommunicator(*this, _logger);
}

bool DynamicCubeSatJob::appl_initialize() {
    const std::lock_guard<Mutex> lock(_initialization_mutex);

    assert(_job_state == UNINITIALIZED || _job_state == SUSPENDED_BEFORE_INITIALIZATION || _job_state == INTERRUPTED_BEFORE_INITIALIZATION);

    // Update _logger
    _logger.setIdentifier("<c-" + std::to_string(_world_rank) + std::string(toStr()) + ">");
    _logger.log(0, "Logger was updated");

    // Check if job was aborted before initialization
    if (_job_state == INTERRUPTED_BEFORE_INITIALIZATION) {
        _logger.log(0, "Job was interrupted before initialization");
        _job_state.store(State::WITHDRAWN);
        return false;
    }

    _logger.log(0, "Started intializing dynamic cube lib");

    DynamicCubeSetup cube_setup(getDescription().getPayloads().at(0), _logger, _params, _sat_result);

    // Initialize dynamic cube lib
    _lib = std::make_unique<DynamicCubeLib>(cube_setup, isRoot());

    _logger.log(0, "Finished intializing dynamic cube lib");

    // If job was suspended before initialization. Respecting INITIALIZING_TO_SUSPENDED
    if (_job_state == SUSPENDED_BEFORE_INITIALIZATION) {
        _job_state.store(SUSPENDED);
    } else {
        _lib->start();
        _job_state.store(WORKING);
    }

    return true;
}

bool DynamicCubeSatJob::appl_doneInitializing() {
    return _job_state != UNINITIALIZED && _job_state != SUSPENDED_BEFORE_INITIALIZATION && _job_state != INTERRUPTED_BEFORE_INITIALIZATION;
}

void DynamicCubeSatJob::appl_updateRole() { assert(Console::fail("Not implemented yet!")); }

void DynamicCubeSatJob::appl_updateDescription(int fromRevision) { assert(Console::fail("Not implemented yet!")); }

void DynamicCubeSatJob::appl_pause() {
    // Manipulation lock is held
    // Consider this when checking for the job state
    // After this returns the job state is set to suspended

    const std::lock_guard<Mutex> lock(_initialization_mutex);

    _logger.log(0, "appl_pause was called");

    if (_job_state == UNINITIALIZED) {
        _job_state = SUSPENDED_BEFORE_INITIALIZATION;

    } else if (_job_state == WORKING) {
        // Synchronously interrupt and join the dynamic cube lib
        _lib->suspend();

        // Release all cubes
        ((NewDynamicCubeCommunicator*)_dynamic_cube_comm)->releaseAll();

        // Share all found failed cubes
        ((FailedAssumptionCommunicator*)_failed_assumption_comm)->release();

        // Set state to suspended
        _job_state = SUSPENDED;
    }
}

void DynamicCubeSatJob::appl_unpause() {
    const std::lock_guard<Mutex> lock(_initialization_mutex);

    _logger.log(0, "appl_unpause was called");

    if (_job_state == SUSPENDED_BEFORE_INITIALIZATION) {
        _job_state = UNINITIALIZED;

    } else if (_job_state == SUSPENDED) {
        _lib->start();
        _job_state = WORKING;
    }
}

void DynamicCubeSatJob::appl_interrupt() {
    const std::lock_guard<Mutex> lock(_initialization_mutex);

    _logger.log(0, "appl_interrupt was called");

    interrupt_and_start_withdrawing();
}

void DynamicCubeSatJob::appl_withdraw() {
    const std::lock_guard<Mutex> lock(_initialization_mutex);

    _logger.log(0, "appl_withdraw was called");

    interrupt_and_start_withdrawing();
}

void DynamicCubeSatJob::interrupt_and_start_withdrawing() {
    if (_job_state == UNINITIALIZED || _job_state == SUSPENDED_BEFORE_INITIALIZATION) {
        _job_state = INTERRUPTED_BEFORE_INITIALIZATION;

    } else if (_job_state == WORKING) {
        _lib->interrupt();

        _job_state = WITHDRAWING;

        _withdraw_thread = std::thread(&DynamicCubeSatJob::withdraw, this);

    } else if (_job_state == SUSPENDED) {
        _job_state = WITHDRAWN;
    }
}

void DynamicCubeSatJob::withdraw() {
    _logger.log(0, "Started withdraw thread");

    // Wait until dynamic cube lib is joined
    _lib->join();

    const std::lock_guard<Mutex> lock(_initialization_mutex);

    _job_state = WITHDRAWN;

    // Delete dynamic cube lib
    _lib.reset();

    _logger.log(0, "Finished withdraw thread");
}

int DynamicCubeSatJob::appl_solveLoop() {
    if (_sat_result != UNKNOWN) {
        _logger.log(0, "Found result %s", _sat_result == 10 ? "SAT" : _sat_result == 20 ? "UNSAT" : "UNKNOWN");

        _result.id = getId();
        _result.result = _sat_result;
        _result.revision = getDescription().getRevision();
        _result.solution.clear();

        return 1;
    }
    // Default case
    return -1;
}

void DynamicCubeSatJob::appl_dumpStats() {}

bool DynamicCubeSatJob::appl_isDestructible() {
    // Allow destruction when this job was not initialized
    // This should not be a race condition, since it is done in the threaded_sat_job.cpp
    return !appl_doneInitializing() || _job_state == State::WITHDRAWN;
}

bool DynamicCubeSatJob::appl_wantsToBeginCommunication() const {
    // Before this method is called isInStateUnsafe({ACTIVE, INITIALIZING_TO_ACTIVE}) is checked
    // Also the same thread that call appl_suspend calls this method -> Job is either active or initializing to active

    // From threaded_sat_job.cpp
    if (_job_comm_period <= 0) return false;

    // Return true on every leaf node in intervals
    // Also on the root node since it may have received cubes and then the job could have shrunken to only the root
    // TODO May be change this by adding all cubes in the root communicator to the root lib

    // Check whether the node is a leaf and the lib is working
    if (isLeaf() && _job_state == WORKING) {
        // Special "timed" conditions for leaf nodes:
        // At least half a second since initialization / reactivation
        if (getAgeSinceActivation() < 0.5 * _job_comm_period) return false;
        // At least params["s"] seconds since last communication
        if (Timer::elapsedSeconds() - _time_of_last_comm < _job_comm_period) return false;

        return true;

    } else {
        return false;
    }
}

void DynamicCubeSatJob::appl_beginCommunication() {
    // Is called by the same thread directly after appl_wantsToBeginCommunication return, therefore no suspension or interruption could have happenend since
    ((NewDynamicCubeCommunicator*)_dynamic_cube_comm)->sendMessageToParent();
    ((FailedAssumptionCommunicator*)_failed_assumption_comm)->gather();

    _time_of_last_comm = Timer::elapsedSeconds();
}

void DynamicCubeSatJob::appl_communicate(int source, JobMessage& msg) {
    // This job must have been active once, otherwise it would never have been in the job tree and would therefore not receive this message
    // !Messages are send to nodes not to job tree positions!
    // Handle message if job was not interrupted
    if (_job_state != INTERRUPTED_BEFORE_INITIALIZATION && _job_state != WITHDRAWING && _job_state != WITHDRAWN) {
        if (NewDynamicCubeCommunicator::isDynamicCubeMessage(msg.tag)) {
            ((NewDynamicCubeCommunicator*)_dynamic_cube_comm)->handle(source, msg);

        } else if (FailedAssumptionCommunicator::isFailedAssumptionMessage(msg.tag)) {
            ((FailedAssumptionCommunicator*)_failed_assumption_comm)->handle(source, msg);

        } else {
            Console::fail("Unknown message");
        }
    }
}

int DynamicCubeSatJob::getDemand(int prevVolume, float elapsedTime) const {
    // Only expand once the root is working, thus always allowing emergency messages to the root nodes lib
    if (_job_state != WORKING)
        return 1;
    else
        return Job::getDemand(prevVolume, elapsedTime);
}

DynamicCubeSatJob::~DynamicCubeSatJob() {
    const std::lock_guard<Mutex> lock(_initialization_mutex);

    _logger.log(0, "Enter destructor");

    // The withdraw thread might still be default constructed, because of an aborted initialization or a prior suspension
    if (_withdraw_thread.joinable()) {
        _withdraw_thread.join();
        _logger.log(0, "Joined cleanup thread");
    }

    // Destroy the communicators
    assert(_dynamic_cube_comm != NULL);
    delete (NewDynamicCubeCommunicator*)_dynamic_cube_comm;

    assert(_failed_assumption_comm != NULL);
    delete (FailedAssumptionCommunicator*)_failed_assumption_comm;

    _logger.log(0, "Exit destructor");
}

/* Methods for clause sharing */

bool DynamicCubeSatJob::isRequesting() {
    // The caller guarantees that the job is active -> active is needed to communicate with parent and children

    // Return false if the lib is not initialized
    if (!appl_doneInitializing()) {
        _logger.log(0, "Lib is not initialized and therefore not requesting");
        return false;

    } else
        return _lib->isRequesting();
}

std::vector<Cube> DynamicCubeSatJob::getCubes(int bias) {
    // The caller guarantees that the job is active -> active is needed to communicate with parent and children

    // Return empty vector if not the lib is not initialized
    if (!appl_doneInitializing()) {
        _logger.log(0, "Lib is not initialized and therefore has no free cubes");
        return std::vector<Cube>();
    }

    return _lib->getCubes(bias);
}

void DynamicCubeSatJob::digestCubes(std::vector<Cube>& cubes) {
    // The caller guarantees that the DynamicCubeSatJob is working
    assert(_job_state == WORKING);

    _lib->digestCubes(cubes);
}

std::vector<Cube> DynamicCubeSatJob::releaseAllCubes() {
    // The caller guarantees that the DynamicCubeSatJob is working
    assert(_job_state == WORKING);

    return _lib->releaseAllCubes();
}

std::vector<int> DynamicCubeSatJob::getFailedAssumptions() {
    // The caller guarantees that the job is active -> active is needed to communicate with parent and children

    // Return empty vector if not the lib is not initialized
    if (!appl_doneInitializing()) {
        _logger.log(0, "Lib is not initialized and therefore has no failed assumptions");
        return std::vector<int>();
    }

    return _lib->getNewFailedAssumptions();
}

void DynamicCubeSatJob::digestFailedAssumptions(std::vector<int>& failed_assumptions) {
    // May only be called after a job was initialized, otherwise some failed assumptions may never be learned
    assert(appl_doneInitializing());

    _lib->digestFailedAssumptions(failed_assumptions);
}