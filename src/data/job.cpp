
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
            _abort_after_initialization(false),
            _done_locally(false), 
            _job_node_ranks(commSize, jobId),
            _has_left_child(false),
            _has_right_child(false)
             {}

void Job::lockJobManipulation() {
    Console::log(Console::VVVERB, "%s : locking job manipulation ...", toStr());
    jobManipulationLock.lock();
    Console::log(Console::VVVERB, "%s : locked job manipulation", toStr());
}
void Job::unlockJobManipulation() {
    jobManipulationLock.unlock();
    Console::log(Console::VVVERB, "%s : unlocked job manipulation", toStr());
}

void Job::setDescription(std::shared_ptr<std::vector<uint8_t>>& data) {

    lockJobManipulation();
    // Explicitly store serialized data s.t. it can be forwarded later
    // without the need to re-serialize the job description
    assert(data != NULL && data->size() > 0);
    _serialized_description = data;
    _description = JobDescription();
    _description.deserialize(*_serialized_description);
    _has_description = true;
    unlockJobManipulation();
}

void Job::addAmendment(std::shared_ptr<std::vector<uint8_t>>& data) {

    lockJobManipulation();
    int oldRevision = _description.getRevision();
    _description.merge(*data);
    appl_updateDescription(oldRevision+1);
    switchState(ACTIVE);
    unlockJobManipulation();
}

void Job::beginInitialization() {
    lockJobManipulation();
    _elapsed_seconds_since_arrival = Timer::elapsedSeconds();
    switchState(INITIALIZING_TO_ACTIVE);
    unlockJobManipulation();
}

void Job::endInitialization() {

    lockJobManipulation();
    if (isInState({PAST})) {
        unlockJobManipulation();
        return;
    }
    _initialized = true;
    JobState oldState = _state;
    switchState(ACTIVE);
    if (_params.getFloatParam("s") > 0)
        _last_job_comm_remainder = (int)(Timer::elapsedSeconds() / _params.getFloatParam("s"));
    _time_of_initialization = Timer::elapsedSeconds();
    if (oldState == INITIALIZING_TO_PAST) {
        unlockJobManipulation();
        terminate();
    } else if (oldState == INITIALIZING_TO_SUSPENDED) {
        unlockJobManipulation();
        suspend();
    } else if (oldState == INITIALIZING_TO_COMMITTED) {
        switchState(COMMITTED);
        unlockJobManipulation();
    } else {
        unlockJobManipulation();
    }
}

void Job::initialize(int index, int rootRank, int parentRank) {

    lockJobManipulation();
    _index = index;
    updateJobNode(0, rootRank);
    updateParentNodeRank(parentRank);
    updateJobNode(_index, _world_rank);
    appl_initialize();
    unlockJobManipulation();
}

void Job::reinitialize(int index, int rootRank, int parentRank) {

    if (!_initialized && !isInitializing()) {
        
        beginInitialization();
        initialize(index, rootRank, parentRank);

    } else {

        lockJobManipulation();
        if (index == _index) {

            // Job of same index as before is resumed
            updateParentNodeRank(parentRank);
            Console::log(Console::INFO, "Resuming solvers of %s", toStr());
            unlockJobManipulation();
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
                unlockJobManipulation();
                suspend();
                appl_updateRole();
                resume();
                lockJobManipulation();
                switchState(ACTIVE);
            } else {
                switchState(INITIALIZING_TO_ACTIVE);
            }
            unlockJobManipulation();
        }
    }
}

void Job::commit(const JobRequest& req) {

    lockJobManipulation();
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
    
    unlockJobManipulation();
}

void Job::uncommit() {

    lockJobManipulation();
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
    unlockJobManipulation();
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
    lockJobManipulation();
    if (isInitializing()) {
        switchState(INITIALIZING_TO_SUSPENDED);
        unlockJobManipulation();
        return;
    }
    assert(isInState({ACTIVE}));
    appl_pause();
    switchState(SUSPENDED);
    unlockJobManipulation();
    Console::log(Console::INFO, "%s : suspended solver", toStr());
}

void Job::resume() {
    lockJobManipulation();
    if (isInitializing()) {
        switchState(INITIALIZING_TO_ACTIVE);
        unlockJobManipulation();        
        return;
    }
    if (!_initialized) {
        unlockJobManipulation();
        initialize(_index, getRootNodeRank(), getParentNodeRank());
    } else {
        appl_unpause();
        switchState(ACTIVE);
        unlockJobManipulation();
        Console::log(Console::INFO, "Resumed solving threads of %s", toStr());
    }
}

void Job::stop() {
    lockJobManipulation();
    if (isInitializing()) {
        switchState(INITIALIZING_TO_PAST);
        unlockJobManipulation();
        return;
    }
    appl_interrupt();
    switchState(STANDBY);
    unlockJobManipulation();
}

void Job::terminate() {

    /*
    if (isInitializing()) {
        switchState(INITIALIZING_TO_PAST);
        _abort_after_initialization = true;
        return;
    } else {
        assert(isInState({ACTIVE, SUSPENDED, STANDBY}));
    }*/

    lockJobManipulation();
    appl_interrupt();
    appl_withdraw();

    // Free up memory
    _description = JobDescription();
    _serialized_description = std::make_shared<std::vector<uint8_t>>();
    _serialized_description->resize(sizeof(int));
    memcpy(_serialized_description->data(), &_id, sizeof(int));

    switchState(PAST);
    unlockJobManipulation();

    Console::log(Console::VERB, "Terminated %s and freed memory.", toStr());
}

int Job::getDemand(int prevVolume) const {
    if (isInState({ACTIVE, INITIALIZING_TO_ACTIVE})) {

        int demand;
        float growthPeriod = _params.getFloatParam("g");
        if (growthPeriod <= 0) {
            // Immediate growth
            demand = _comm_size;
        } else {
            // Periodic growth, limited by total amount of worker nodes
            int numGrowths = (int) ((Timer::elapsedSeconds()-_time_of_initialization) / growthPeriod);
            demand = std::min(_comm_size, (int)std::pow(2, numGrowths + 1) - 1);
        }
        // Limit demand if desired
        if (_params.getIntParam("md") > 0) {
            demand = std::min(demand, _params.getIntParam("md"));
        }
        return demand;
        
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
    return isInState({ACTIVE, INITIALIZING_TO_ACTIVE}) && !hasLeftChild() && !hasRightChild() 
            && getJobCommEpoch() > _last_job_comm_remainder;
}

void Job::communicate() {
    assert(_params.getFloatParam("s") > 0.0f);
    _last_job_comm_remainder = getJobCommEpoch();
    appl_beginCommunication();
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

Job::~Job() {
    
    lockJobManipulation();
    
    _serialized_description.reset();
    _serialized_description = NULL;

    if (_initializer_thread != NULL) {
        _initializer_thread->join();
        _initializer_thread.release();
        _initializer_thread = NULL;
    }

    unlockJobManipulation();
    
    Console::log(Console::VERB, "Leaving destructor of %s.", toStr());
}