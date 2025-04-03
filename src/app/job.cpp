
#include <cmath>
#include <cstdlib>
#include <limits>

#include "app/job.hpp"
#include "util/logger.hpp"
#include "util/sys/timer.hpp"
#include "comm/msgtags.h"


Job::Job(const Parameters& params, const JobSetup& setup, AppMessageTable& appMsgTable) :
            _params(params), 
            _id(setup.jobId),
            _name("#" + std::to_string(setup.jobId)),
            _application_id(setup.applicationId),
            _incremental(setup.incremental),
            _time_of_arrival(Timer::elapsedSeconds()), 
            _state(INACTIVE),
            _app_msg_subscription(appMsgTable, this),
            _job_tree(setup.commSize, setup.worldRank, _app_msg_subscription.getContextId(), 
                setup.jobId, params.useDormantChildren()), 
            _comm(_id, _job_tree, params.jobCommUpdatePeriod()) {

    _growth_period = _params.growthPeriod();
    _continuous_growth = _params.continuousGrowth();
    _max_demand = _params.maxDemand();
    _threads_per_job = _params.numThreadsPerProcess();
}

void Job::updateJobTree(int index, int rootRank, ctx_id_t rootContextId, int parentRank, ctx_id_t parentContextId) {
    if (index == 0) rootRank = -1;
    _name = "#" + std::to_string(_id) + ":" + std::to_string(index);
    _job_tree.update(index, rootRank, rootContextId, parentRank, parentContextId);
}

void Job::commit(const JobRequest& req) {
    assert(getState() != ACTIVE);
    assert(getState() != PAST);
    _balancing_epoch_of_last_commitment = req.balancingEpoch;
    _job_tree.clearJobNodeUpdates();
    updateJobTree(req.requestedNodeIndex, req.rootRank, req.rootContextId, 
        req.requestingNodeRank, req.requestingNodeContextId);
    updateJobBalancingEpoch(_balancing_epoch_of_last_commitment);
    _commitment = req;
}

std::optional<JobRequest> Job::uncommit() {
    assert(getState() != ACTIVE);
    std::optional<JobRequest> optReq(std::move(_commitment));
    _commitment.reset();
    return optReq;
}

void Job::pushRevision(const std::shared_ptr<std::vector<uint8_t>>& data) {

    const int prevGroupId = _description.getGroupId();
    _description.deserialize(data);
    _priority = _description.getPriority();
    if (_description.getMaxDemand() > 0) {
        // Set max. demand to more restrictive number
        // among global and job-internal limit
        _max_demand = _max_demand == 0 ? 
            _description.getMaxDemand() // no global max. demand defined
            : std::min(_max_demand, _description.getMaxDemand()); // both limits defined
    }

    _sum_of_description_sizes += data->size() / sizeof(int);
    if (_params.maxLiteralsPerThread() > 0 && _sum_of_description_sizes > _params.maxLiteralsPerThread()) {
        
        // Solver literal threshold exceeded: reduce number of solvers for this job
        long maxAllowedLiterals = _threads_per_job * (long)_params.maxLiteralsPerThread();
        int optNumThreads = std::floor(((double)maxAllowedLiterals) / _sum_of_description_sizes);
        _threads_per_job = std::max(1, optNumThreads);
        LOG(V3_VERB, "%s : literal threshold exceeded - cut down #threads to %i\n", toStr(), _threads_per_job);
    }

    if (_description.getGroupId() != prevGroupId) {
        assert(prevGroupId == 0 || log_return_false("[ERROR] Trying to change group ID %i => %i: "
            "Only a single group ID is allowed throughout all revisions of an incremental job!\n",
            prevGroupId, _description.getGroupId()));
        _rev_to_reach_for_group_id = _description.getRevision();
    }

    _has_description = true;
    _result.reset();
    if (getDescription().getAppConfiguration().map.count("__growprd")) {
        _growth_period = atof(getDescription().getAppConfiguration().map.at("__growprd").c_str());
    }
}

void Job::start() {
    assertState(INACTIVE);
    if (_time_of_activation <= 0) {
        _time_of_activation = Timer::elapsedSecondsCached();
        _time_of_increment_activation = _time_of_activation;
    }
    _time_of_last_limit_check = Timer::elapsedSecondsCached();
    _volume = std::max(1, _volume);
    _state = ACTIVE;
    LOG(V4_VVER, "%s : new job node starting\n", toStr());
    appl_start();
}

void Job::suspend() {
    assertState(ACTIVE);
    _state = SUSPENDED;
    appl_suspend();
    _job_tree.unsetLeftChild();
    _job_tree.unsetRightChild();
    LOG(V4_VVER, "%s : suspended solver\n", toStr());
    _request_to_multiply_left.reset();
    _request_to_multiply_right.reset();
    updateVolumeAndUsedCpu(getVolume());
}

void Job::resume() {
    assertState(SUSPENDED);
    _volume = std::max(1, _volume);
    _state = ACTIVE;
    _time_of_increment_activation = Timer::elapsedSecondsCached();
    appl_resume();
    LOG(V4_VVER, "%s : resumed solver\n", toStr());
    _time_of_last_limit_check = Timer::elapsedSecondsCached();
}

void Job::terminate() {
    if (_state == ACTIVE) 
        updateVolumeAndUsedCpu(getVolume());

    _state = PAST;
    
    appl_terminate();

    _job_tree.unsetLeftChild();
    _job_tree.unsetRightChild();
    _request_to_multiply_left.reset();
    _request_to_multiply_right.reset();

    _time_of_abort = Timer::elapsedSeconds();
    LOG(V4_VVER, "%s : terminated\n", toStr());
}

bool Job::isDestructible() {
    bool destructible;
    if (getState() != PAST) destructible = false;
    else destructible = appl_isDestructible();

    // The job signalling to be destructible means that it does not need to
    // (and is not allowed to) process any further application messages.
    // Therefore, we immediately unregister it from the app. message table.
    if (destructible) finalizeCommunication();

    return destructible;
}

int Job::getDemand() const {
    
    if (_state != ACTIVE) {
        return _commitment.has_value() ? 1 : 0;
    } 
    
    int commSize = _job_tree.getCommSize();
    int demand; 
    if (_growth_period <= 0) {
        // Immediate growth
        demand = commSize;
    } else {
        if (_time_of_increment_activation <= 0) demand = 1;
        else {
            double t = Timer::elapsedSecondsCached()-_time_of_increment_activation;
            
            // Continuous growth
            double numPeriods = std::min(t/_growth_period, 28.0); // overflow protection
            if (!_continuous_growth) {
                // Discrete periodic growth
                int intPeriods = std::floor(numPeriods);
                demand = 1;
                for (int i = 0; demand < commSize && i < intPeriods; i++) demand = 2*demand+1;
                demand = std::min(commSize, demand);
            } else {
                // d(0) := 1; d := 2d+1 every <growthPeriod> seconds
                //demand = std::min(commSize, (int)std::pow(2, numPeriods + 1) - 1);
                // quadratic growth
                if (t / _growth_period >= 1000) demand = commSize;
                else demand = std::min(commSize, std::max(1,
                    (int) std::round((t/_growth_period) * (t/_growth_period))));
            }
        }
    }

    // Limit demand if desired
    if (_max_demand > 0) {
        demand = std::min(demand, _max_demand);
    }
    return demand;
}

double Job::getTemperature() const {

    double baseTemp = 0.95;
    double decay = 0.99; // higher means slower convergence

    int age = (int) (Timer::elapsedSecondsCached()-_time_of_activation);
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

JobResult& Job::getResult() {
    if (!_result.has_value()) _result = std::move(appl_getResult());
    JobResult& result = _result.value();
    assert(result.id >= 1);
    assert(result.hasSerialization());
    return result;
}

void Job::communicate() {
    if (_state == ACTIVE && _comm.wantsToAggregate() && _comm.isAggregating())
        _comm.beginAggregation();
    appl_communicate();
}

void Job::communicate(int source, int mpiTag, JobMessage& msg) {
    if (_state == ACTIVE && _comm.handle(msg)) {
        if (msg.tag == MSG_BROADCAST_RANKLIST && _job_tree.isRoot()) {
            // Check size of job comm compared to scheduler's job volume
            if (_comm.size() != getVolume()) {
                LOG(V1_WARN, "[WARN] %s job tree has size %i/%i\n", toStr(), _comm.size(), getVolume());
            }
        }
    } else {
        appl_communicate(source, mpiTag, msg);
    }
}

bool Job::hasAllDescriptionsForSolving(int& missingOrIncompleteRevIdx) {
    if (!hasDescription()) {
        missingOrIncompleteRevIdx = 0;
        return false;
    }
    for (int r = _last_usable_revision+1; r <= getRevision(); r++) {
        if (getDescription().isRevisionIncomplete(r)) {
            if (canHandleIncompleteRevision(r)) {
                _last_usable_revision = r;
                continue;
            }
            missingOrIncompleteRevIdx = r;
            return false;
        }
    }
    if (getRevision() < getDesiredRevision()) {
        missingOrIncompleteRevIdx = getRevision()+1;
        return false;
    }
    return true;
}
