
#include <map>
#include <thread>
#include <cmath>
#include <limits>

#include "assert.h"
#include "data/job.h"
#include "util/console.h"
#include "util/timer.h"

void logMutex(const char* msg) {
    Console::log(Console::VVVERB, msg);
}

Job::Job(Parameters& params, int commSize, int worldRank, int jobId, EpochCounter& epochCounter) :
            _params(params), 
            _comm_size(commSize), 
            _world_rank(worldRank), 
            _id(jobId),
            _name("#" + std::to_string(jobId)),
            _epoch_counter(epochCounter), 
            _epoch_of_arrival(epochCounter.getEpoch()), 
            _elapsed_seconds_since_arrival(Timer::elapsedSeconds()), 
            _state(NONE),
            _has_description(false), 
            _initialized(false), 
            //_job_manipulation_lock(VerboseMutex("JobManip#" + std::to_string(_id), &logMutex)),
            _job_comm_period(params.getFloatParam("s")),
            _job_node_ranks(commSize, jobId),
            _has_left_child(false),
            _has_right_child(false)
             {}

void Job::lockJobManipulation() {
    _job_manipulation_lock.lock();
}
void Job::unlockJobManipulation() {
    _job_manipulation_lock.unlock();
}

void Job::setDescription(std::shared_ptr<std::vector<uint8_t>>& data) {

    auto lock = _job_manipulation_lock.getLock();
    // Explicitly store serialized data s.t. it can be forwarded later
    // without the need to re-serialize the job description
    assert(data != NULL && data->size() > 0);
    _serialized_description = data;
    _description = JobDescription();
    _description.deserialize(*_serialized_description);
    _has_description = true;
}

void Job::addAmendment(std::shared_ptr<std::vector<uint8_t>>& data) {

    auto lock = _job_manipulation_lock.getLock();
    int oldRevision = _description.getRevision();
    _description.merge(*data);
    appl_updateDescription(oldRevision+1);
    switchState(ACTIVE);
}

void Job::beginInitialization() {
    auto lock = _job_manipulation_lock.getLock();
    _elapsed_seconds_since_arrival = Timer::elapsedSeconds();
    switchState(INITIALIZING_TO_ACTIVE);
}

void Job::endInitialization() {

    if (isInState({PAST})) {
        return;
    }
    auto lock = _job_manipulation_lock.getLock();
    _initialized = true;
    JobState oldState = _state;
    switchState(ACTIVE);
    if (_job_comm_period > 0)
        _last_job_comm_remainder = (int)(Timer::elapsedSeconds() / _job_comm_period);
    _time_of_initialization = Timer::elapsedSeconds();
    if (oldState == INITIALIZING_TO_PAST) {
        lock.release();
        terminate();
    } else if (oldState == INITIALIZING_TO_SUSPENDED) {
        lock.release();
        suspend();
    } else if (oldState == INITIALIZING_TO_COMMITTED) {
        switchState(COMMITTED);
    }
}

void Job::initialize() {
    bool success = appl_initialize();
    if (!success) {
        endInitialization();
        appl_withdraw();
    }
}

void Job::initialize(int index, int rootRank, int parentRank) {

    auto lock = _job_manipulation_lock.getLock();
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

        lockJobManipulation();
        if (index == _index) {

            // Job of same index as before is resumed
            updateParentNodeRank(parentRank);
            Console::log(Console::INFO, "%s : resuming solvers", toStr());
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
                Console::log(Console::INFO, "%s : restarting solvers", toStr());
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
    
    auto lock = _job_manipulation_lock.getLock();
    assert(isNotInStateUnsafe({ACTIVE, COMMITTED}) 
        || Console::fail("State of %s : %s", toStr(), jobStateToStr()));

    _index = req.requestedNodeIndex;
    updateJobNode(_index, _world_rank);
    if (_index > 0) {
        updateJobNode(0, req.rootRank);
    }
    updateParentNodeRank(req.requestingNodeRank);

    if (isInitializingUnsafe())
        switchState(INITIALIZING_TO_COMMITTED);
    else
        switchState(COMMITTED);
}

void Job::uncommit() {

    auto lock = _job_manipulation_lock.getLock();
    assert(isInStateUnsafe({COMMITTED, INITIALIZING_TO_COMMITTED}));

    if (_initialized) {
        switchState(SUSPENDED);
    } else if (isInitializingUnsafe()) {
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
    auto lock = _job_manipulation_lock.getLock();
    if (isInitializingUnsafe()) {
        switchState(INITIALIZING_TO_SUSPENDED);
        return;
    }
    assert(isInStateUnsafe({ACTIVE}));
    appl_pause();
    switchState(SUSPENDED);
    Console::log(Console::INFO, "%s : suspended solver", toStr());
}

void Job::resume() {
    auto lock = _job_manipulation_lock.getLock();
    if (isInitializingUnsafe()) {
        switchState(INITIALIZING_TO_ACTIVE);
        return;
    }
    if (!_initialized) {
        lock.release();
        initialize(_index, getRootNodeRank(), getParentNodeRank());
    } else {
        appl_unpause();
        switchState(ACTIVE);
        Console::log(Console::INFO, "%s : resumed solving threads", toStr());
    }
}

void Job::stop() {
    auto lock = _job_manipulation_lock.getLock();
    if (isInitializingUnsafe()) {
        switchState(INITIALIZING_TO_PAST);
        return;
    }
    appl_interrupt();
    switchState(STANDBY);
}

void Job::terminate() {

    auto lock = _job_manipulation_lock.getLock();
    appl_interrupt();
    appl_withdraw();

    unsetLeftChild();
    unsetRightChild();

    // Free up memory
    _description = JobDescription();
    _serialized_description = std::make_shared<std::vector<uint8_t>>();
    _serialized_description->resize(sizeof(int));
    memcpy(_serialized_description->data(), &_id, sizeof(int));
    _time_of_abort = Timer::elapsedSeconds();

    switchState(PAST);
    Console::log(Console::VERB, "%s : terminated, memory freed", toStr());
}

bool Job::isDestructible() {
    return !isInitializing() && appl_isDestructible();
}

int Job::getDemand(int prevVolume) const {
    if (isInState({ACTIVE, INITIALIZING_TO_ACTIVE})) {

        int demand; 
        float growthPeriod = _params.getFloatParam("g");
        if (growthPeriod <= 0) {
            // Immediate growth
            demand = _comm_size;
        } else {
            if (_time_of_initialization <= 0) demand = 1;
            else {
                float t = Timer::elapsedSeconds()-_time_of_initialization;
                
                // Continuous growth
                float numPeriods = t/growthPeriod;
                if (!_params.isSet("cg")) {
                    // Discrete, periodic growth
                    numPeriods = std::floor(numPeriods);
                }
                // d(0) := 1; d := 2d+1 every <growthPeriod> seconds
                demand = std::min(_comm_size, (int)std::pow(2, numPeriods + 1) - 1);
            }
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

double Job::getTemperature() const {

    double baseTemp = 0.95;
    double decay = 0.99; // higher means slower convergence

    int age = (int) (Timer::elapsedSeconds()-_time_of_initialization);
    double eps = 2*std::numeric_limits<double>::epsilon();

    // Start with temperature 1.0, exponentially converge towards baseTemp 
    double temp = baseTemp + (1-baseTemp) * std::pow(decay, age+1);
    
    // Check if machine precision range is reached, if not reached yet
    if (_age_of_const_cooldown < 0 && _last_temperature - temp <= eps) {
        _age_of_const_cooldown = age;
    }
    // Was limit already reached?
    if (_age_of_const_cooldown >= 0) {
        // indefinitely cool down job by machine precision epsilon
        return baseTemp + (1-baseTemp) * std::pow(decay, _age_of_const_cooldown+1) - (age-_age_of_const_cooldown+1)*eps;
    } else {
        // Use normal calculated temperature
        _last_temperature = temp;
        return temp;
    }
}

const JobResult& Job::getResult() const {
    assert(_result.id >= 0); 
    return _result;
}

bool Job::wantsToCommunicate() const {
    if (_job_comm_period <= 0.0f) {
        // No communication
        return false;
    }
    // Active leaf node initiates communication if s seconds have passed since last one
    return isActive() && !hasLeftChild() && !hasRightChild() 
            && getJobCommEpoch() > _last_job_comm_remainder;
}

void Job::communicate() {
    assert(_job_comm_period > 0.0f);
    _last_job_comm_remainder = getJobCommEpoch();
    appl_beginCommunication();
}

bool Job::isInState(std::initializer_list<JobState> list) const {
    auto lock = _job_manipulation_lock.getLock();
    return isInStateUnsafe(list);
}
bool Job::isNotInState(std::initializer_list<JobState> list) const {
    auto lock = _job_manipulation_lock.getLock();
    return isNotInStateUnsafe(list);
}
bool Job::isInStateUnsafe(std::initializer_list<JobState> list) const {
    for (JobState state : list) {
        if (state == _state)
            return true;
    }
    return false;
}
bool Job::isNotInStateUnsafe(std::initializer_list<JobState> list) const {
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

void Job::setForgetting() {
    auto lock = _job_manipulation_lock.getLock();
    if (!isForgetting()) switchState(FORGETTING);
}

Job::~Job() {
    
    auto lock = _job_manipulation_lock.getLock();
    
    _serialized_description.reset();
    _serialized_description = NULL;

    if (_initializer_thread != NULL) {
        if (_initializer_thread->joinable()) {
            _initializer_thread->join();
        }
        _initializer_thread.release();
        _initializer_thread = NULL;
    }
    
    Console::log(Console::VERB, "destructed %s", toStr());
}