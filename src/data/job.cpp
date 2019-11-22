
#include <map>
#include <thread>
#include <cmath>

#include "assert.h"
#include "data/job.h"
#include "util/console.h"
#include "util/timer.h"

Job::Job(Parameters& params, int commSize, int worldRank, int jobId, EpochCounter& epochCounter) :
            _params(params), 
            _comm_size(commSize), 
            _world_rank(worldRank), 
            _id(jobId),
            _epoch_counter(epochCounter), 
            _epoch_of_arrival(epochCounter.getEpoch()), 
            _elapsed_seconds_since_arrival(Timer::elapsedSeconds()), 
            _state(NONE),
            _has_description(false), 
            _initialized(false), 
            _done_locally(false), 
            _job_node_ranks(commSize, jobId),
            _has_left_child(false),
            _has_right_child(false)
             {}

void Job::store(std::shared_ptr<std::vector<uint8_t>>& data) {
    setDescription(data);
    if (isInState({NONE})) {
        _index = -1;
        switchState(STORED);
    }
}

void Job::setDescription(std::shared_ptr<std::vector<uint8_t>>& data) {

    // Explicitly store serialized data s.t. it can be forwarded later
    // without the need to re-serialize the job description
    assert(data != NULL && data->size() > 0);
    _serialized_description = data;
    _description = JobDescription();
    _description.deserialize(*_serialized_description);
    _has_description = true;
}

void Job::addAmendment(std::shared_ptr<std::vector<uint8_t>>& data) {
    int oldRevision = _description.getRevision();
    _description.merge(*data);
    updateDescription(oldRevision+1);
    switchState(ACTIVE);
}

void Job::initialize() {
    endInitialization();
}

void Job::beginInitialization() {
    _elapsed_seconds_since_arrival = Timer::elapsedSeconds();
    switchState(INITIALIZING_TO_ACTIVE);
}

void Job::endInitialization() {
    _initialized = true;
    JobState oldState = _state;
    switchState(ACTIVE);
    if (_params.getFloatParam("s") > 0)
        _last_job_comm_remainder = (int)(Timer::elapsedSeconds() / _params.getFloatParam("s"));
    _time_of_initialization = Timer::elapsedSeconds();
    if (oldState == INITIALIZING_TO_PAST) {
        terminate();
    } else if (oldState == INITIALIZING_TO_SUSPENDED) {
        suspend();
    } else if (oldState == INITIALIZING_TO_COMMITTED) {
        switchState(COMMITTED);
    }
}

void Job::initialize(int index, int rootRank, int parentRank) {
    _index = index;
    updateJobNode(0, rootRank);
    updateParentNodeRank(parentRank);
    updateJobNode(_index, _world_rank);
    initialize();
}

void Job::reinitialize(int index, int rootRank, int parentRank) {

    if (!_initialized && !isInitializing()) {
        
        beginInitialization();
        initialize(index, rootRank, parentRank);

    } else {

        if (index == _index) {

            // Job of same index as before is resumed
            updateParentNodeRank(parentRank);
            Console::log(Console::INFO, "Resuming solvers of %s", toStr());
            resume();

        } else {
            _index = index;

            // Restart clean permutation
            _job_node_ranks.clear();
            updateJobNode(0, rootRank);
            updateParentNodeRank(parentRank);
            updateJobNode(index, _world_rank);

            if (_initialized) {
                Console::log(Console::INFO, "Restarting solvers of %s", toStr());
                suspend();
                updateRole();
                resume();
                switchState(ACTIVE);
            } else {
                switchState(INITIALIZING_TO_ACTIVE);
            }

        }
    }
}

void Job::commit(const JobRequest& req) {

    assert(isNotInState({ACTIVE, COMMITTED}) 
        || Console::fail("State of %s : %s", toStr(), jobStateToStr()));

    _index = req.requestedNodeIndex;
    updateJobNode(_index, _world_rank);
    if (_index > 0) {
        updateJobNode(0, req.rootRank);
    }
    updateParentNodeRank(req.requestingNodeRank);

    if (isInitializing())
        switchState(INITIALIZING_TO_COMMITTED);
    else
        switchState(COMMITTED);
}

void Job::uncommit(const JobRequest& req) {

    assert(isInState({COMMITTED, INITIALIZING_TO_COMMITTED}));

    if (_initialized) {
        switchState(SUSPENDED);
    } else if (isInitializing()) {
        switchState(INITIALIZING_TO_SUSPENDED);
    } else if (_has_description) {
        switchState(STORED);
    } else {
        switchState(NONE);
    }
    _job_node_ranks.clear();
}

void Job::setLeftChild(int rank) {
    _has_left_child = true;
    updateJobNode(getLeftChildIndex(), rank);
}
void Job::setRightChild(int rank) {
    _has_right_child = true;
    updateJobNode(getRightChildIndex(), rank);
}

void Job::suspend() {
    if (isInitializing()) {
        switchState(INITIALIZING_TO_SUSPENDED);
        return;
    }
    assert(isInState({ACTIVE}));
    pause();
    switchState(SUSPENDED);
    Console::log(Console::INFO, "%s : suspended solver", toStr());
}

void Job::resume() {
    if (isInitializing()) {
        switchState(INITIALIZING_TO_ACTIVE);
        return;
    }
    if (!_initialized) {
        initialize(_index, getRootNodeRank(), getParentNodeRank());
    } else {
        unpause();
        switchState(ACTIVE);
        Console::log(Console::INFO, "Resumed solving threads of %s", toStr());
    }
}

void Job::stop() {
    if (isInitializing()) {
        switchState(INITIALIZING_TO_PAST);
        return;
    } else {
        assert(isInState({ACTIVE, SUSPENDED}));
    }
    interrupt();
    switchState(STANDBY);
}

void Job::terminate() {

    if (isInitializing()) {
        switchState(INITIALIZING_TO_PAST);
        _abort_after_initialization = true;
        return;
    } else {
        assert(isInState({ACTIVE, SUSPENDED, STANDBY}));
    }

    interrupt();
    withdraw();

    switchState(PAST);
}

int Job::getDemand(int prevVolume) const {
    if (isInState({ACTIVE, INITIALIZING_TO_ACTIVE})) {

        float growthPeriod = _params.getFloatParam("g");
        if (growthPeriod <= 0) {
            // Immediate growth
            return _comm_size;
        }
        int numGrowths = (int) ((Timer::elapsedSeconds()-_time_of_initialization) / growthPeriod);
        return std::min(_comm_size, (int)std::pow(2, numGrowths + 1) - 1);
        
    } else {
        // "frozen"
        return prevVolume;
    }
}

const JobResult& Job::getResult() const {
    assert(_result.id >= 0); 
    return _result;
}

bool Job::wantsToCommunicate() const {
    if (_params.getFloatParam("s") <= 0.0f) {
        // No communication
        return false;
    }
    // Active leaf node initiates communication if s seconds have passed since last one
    return isInState({ACTIVE}) && !hasLeftChild() && !hasRightChild() 
            && (int)(Timer::elapsedSeconds() / _params.getFloatParam("s")) > _last_job_comm_remainder;
}

void Job::communicate() {
    assert(_params.getFloatParam("s") > 0.0f);
    _last_job_comm_remainder = (int)(Timer::elapsedSeconds() / _params.getFloatParam("s"));
    beginCommunication();
}

bool Job::isInState(std::initializer_list<JobState> list) const {
    for (JobState state : list) {
        if (state == _state)
            return true;
    }
    return false;
}
bool Job::isNotInState(std::initializer_list<JobState> list) const {
    for (JobState state : list) {
        if (state == _state)
            return false;
    }
    return true;
}

void Job::switchState(JobState state) {
    JobState oldState = _state;
    _state = state;
    Console::log(Console::VERB, "%s : state transition \"%s\" => \"%s\"", toStr(), 
        jobStateStrings[oldState], jobStateStrings[state]); 
}