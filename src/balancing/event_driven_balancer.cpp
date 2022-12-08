
#include <climits>

#include "event_driven_balancer.hpp"
#include "util/random.hpp"
#include "app/job.hpp"
#include "volume_calculator.hpp"
#include "util/data_statistics.hpp"

EventDrivenBalancer::EventDrivenBalancer(MPI_Comm& comm, Parameters& params) : _comm(comm), _params(params) {

    int size = MyMpi::size(_comm);
    int myRank = MyMpi::rank(_comm);

    // Root rank
    _root_rank = 0;

    if (size == 1) return;

    // Parent rank
    {
        int exp = 2;
        if (myRank == 0) _parent_rank = 0;
        else while (true) {
            if (myRank % exp == exp/2 
                    && myRank - exp/2 >= 0) {
                _parent_rank = myRank - exp/2;
                break;
            }
            exp *= 2;
        }
    }

    // Child ranks
    {
        int exp = 1; while (exp < size) exp *= 2;
        while (true) {
            if (myRank % exp == 0) {
                int child = myRank + exp/2;
                if (child < size) {
                    _child_ranks.push_back(child);
                } 
            }
            exp /= 2;
            if (exp == 1) break;
        }
    }

    LOG(V5_DEBG, "BLC_TREE parent: %i\n", getParentRank());
    LOG(V5_DEBG, "BLC_TREE children: ");
    for (int child : getChildRanks()) LOG_OMIT_PREFIX(V5_DEBG, "%i ", child);
    LOG_OMIT_PREFIX(V5_DEBG, ".\n");
}

void EventDrivenBalancer::setVolumeUpdateCallback(std::function<void(int, int, float)> callback) {
    _volume_update_callback = callback;
}

void EventDrivenBalancer::setBalancingDoneCallback(std::function<void()> callback) {
    _balancing_done_callback = callback;
}

void EventDrivenBalancer::onProbe(int jobId) {
    _local_jobs.insert(jobId);
    pushEvent(Event({
        jobId, /*jobRootEpoch=*/1, /*demand=*/1, /*priority=*/0.01
    }), /*recordLatency=*/false);
}

void EventDrivenBalancer::onActivate(const Job& job, int demand) {
    
    if (_active_job_id == job.getId()) {
        if (job.getJobTree().isRoot()) onDemandChange(job, demand);
        return;
    }
    _active_job_id = job.getId();
    _local_jobs.insert(job.getId());
    
    if (!job.getJobTree().isRoot()) return;
    
    if (!_job_root_epochs.count(job.getId())) _job_root_epochs[job.getId()] = 1;
    /*
    // Do NOT push this event because it was already pushed at onProbe()
    // (with an improper priority, but this does not matter due to atomic demand)
    pushEvent(Event({
        job.getId(), ++_job_root_epochs[job.getId()], std::max(1, demand), job.getPriority()
    }));
    */
}

void EventDrivenBalancer::onDemandChange(const Job& job, int demand) {

    assert(_active_job_id == job.getId());
    assert(job.getJobTree().isRoot());
    assert(_job_root_epochs.at(job.getId()) > 0);

    pushEvent(Event({
        job.getId(), ++_job_root_epochs[job.getId()], demand, job.getPriority()
    }));
}

void EventDrivenBalancer::onSuspend(const Job& job) {

    if (_active_job_id == job.getId()) _active_job_id = -1;
    if (!job.getJobTree().isRoot()) return;

    assert(_job_root_epochs.at(job.getId()) > 0);
    pushEvent(Event({
        job.getId(), ++_job_root_epochs[job.getId()], /*demand=*/1, job.getPriority()
    }));
}

void EventDrivenBalancer::onTerminate(const Job& job) {

    if (_active_job_id == job.getId()) {
        _active_job_id = -1;
        _pending_entries.clear();
        _local_jobs.erase(job.getId());
    } 
    if (!job.getJobTree().isRoot()) return;
    
    if (!_job_root_epochs.count(job.getId())) return;
    assert(_job_root_epochs.at(job.getId()) > 0);
    pushEvent(Event({
        job.getId(), /*jobEpoch=*/INT_MAX, /*demand=*/0, /*priority=*/0 
    }), /*recordLatency=*/false);
    _job_root_epochs.erase(job.getId());

    if (!_balancing_latencies.count(job.getId())) return;
    auto& latencies = _balancing_latencies[job.getId()];
    if (!latencies.empty()) {
        std::sort(latencies.begin(), latencies.end());
        float avgLatency = std::accumulate(latencies.begin(), latencies.end(), 0.0f) / latencies.size();
        LOG(V3_VERB, "%s balancing latency={num:%i min:%.5f med:%.5f avg:%.5f max:%.5f}\n", 
            job.toStr(), latencies.size(), latencies.front(), latencies[latencies.size()/2], avgLatency, latencies.back());
        _past_balancing_latencies.push_back(std::move(latencies));
    }
    _balancing_latencies.erase(job.getId());
}

void EventDrivenBalancer::pushEvent(const Event& event, bool recordLatency) {
    bool inserted = _diffs.insertIfNovel(event);
    if (inserted) {
        if (_pending_entries.count(event.jobId)) {
            // There is a pending event for this job that now becomes obsolete:
            // attribute max. latency
            _balancing_latencies[event.jobId].push_back(Timer::elapsedSeconds() - _pending_entries[event.jobId].second);
        }
        if (recordLatency) {
            _pending_entries[event.jobId] = std::pair<int, float>(event.epoch, Timer::elapsedSeconds());
        }
        LOG(V4_VVER, "BLC insert (%i,%i,%.3f)\n", event.jobId, event.demand, event.priority);
        advance();
    }
}

void EventDrivenBalancer::advance() {
    // Have anything to reduce?
    if (_diffs.isEmpty()) return;

    // Is ready to perform balancing again?
    if (!_periodic_balancing.ready(Timer::elapsedSecondsCached())) return;

    EventMap m = std::move(_diffs);
    _diffs.clear();
    handleData(m, MSG_REDUCE_DATA, /*checkedReady=*/true);
}

void EventDrivenBalancer::handle(MessageHandle& handle) {
    EventMap data = Serializable::get<EventMap>(handle.getRecvData());
    handleData(data, handle.tag, /*checkedReady=*/false);
}

void EventDrivenBalancer::handleData(EventMap& data, int tag, bool checkedReady) {
    if (tag == MSG_REDUCE_DATA) {
        _diffs.updateBy(data);
        if (checkedReady || _periodic_balancing.ready()) {
            if (isRoot(MyMpi::rank(_comm))) {
                // Switch to broadcast, continue below @ other branch
                _diffs.setGlobalEpoch(_balancing_epoch+1);
                tag = MSG_BROADCAST_DATA;
                handleData(_diffs, MSG_BROADCAST_DATA, /*checkedReady=*/true);
            } else { 
                // send diff upwards
                MyMpi::isend(getParentRank(), MSG_REDUCE_DATA, _diffs);
                _diffs.clear();
            }
        }
    } else if (tag == MSG_BROADCAST_DATA) {
        // Inner node: Broadcast further downwards
        if (!isLeaf(MyMpi::rank(_comm))) {
            const auto packed = data.serialize(); 
            for (auto child : getChildRanks()) {
                MyMpi::isendCopy(child, MSG_BROADCAST_DATA, packed);
            }
        }
        // Digest locally
        digest(data);
    }
}

void EventDrivenBalancer::digest(const EventMap& data) {
    
    LOG(V5_DEBG, "BLC DIGEST epoch=%ld size=%ld\n", data.getGlobalEpoch(), data.getEntries().size());
    LOG(V5_DEBG, "BLC DIGEST diff=%s\n", _diffs.toStr().c_str());
    LOG(V5_DEBG, "BLC DIGEST data=%s\n", data.toStr().c_str());
    LOG(V5_DEBG, "BLC DIGEST states_pre=%s\n", _states.toStr().c_str());

    _states.updateBy(data);
    _balancing_epoch = data.getGlobalEpoch();

    LOG(V5_DEBG, "BLC DIGEST states_post=%s\n", _states.toStr().c_str());

    computeBalancingResult();

    // Filter local diffs by the new "global" state.
    size_t diffSize = _diffs.getEntries().size();
    _diffs.filterBy(_states);

    LOG(V5_DEBG, "BLC digest %i diffs, %i/%i local diffs remaining\n", 
            data.getEntries().size(), _diffs.getEntries().size(), diffSize);
    _states.removeOldZeros();
}

void EventDrivenBalancer::computeBalancingResult() {

    int rank = MyMpi::rank(_comm);
    //int verb = rank == 0 ? V4_VVER : V6_DEBGV;
    _job_volumes.clear();

    if (_states.isEmpty()) return;

    if (rank == 0) LOG(V5_DEBG, "BLC: calc result\n");

    VolumeCalculator calc(_states, _params, MyMpi::size(_comm), /*logging=*/rank == 0);
    calc.calculateResult();

    std::string msg = "";
    for (const auto& entry : calc.getEntries()) {
        if (rank == 0)
            msg += std::to_string(entry.jobId) + ":" + std::to_string(entry.volume) + " ";
        
        _job_volumes[entry.jobId] = entry.volume;
        float elapsed = 0;

        // My active job?
        if (entry.jobId == _active_job_id) {
            // Did I fire an event for this job which is not yet fulfilled?
            if (_pending_entries.count(_active_job_id)) {
                auto it = _pending_entries.find(_active_job_id);
                auto& [epoch, time] = it->second;
                // Does the event's epoch fit to the received epoch?
                if (epoch == _states.getEntries().at(_active_job_id).epoch) {
                    // -- Yes: Measure latency, remove pending event
                    elapsed = Timer::elapsedSeconds() - time;
                    _balancing_latencies[entry.jobId].push_back(elapsed);
                    _pending_entries.erase(it);
                }
            }
        }
        
        // Trigger balancing callback?
        if (_local_jobs.count(entry.jobId))
            _volume_update_callback(entry.jobId, entry.volume, elapsed);
    }

    // also call callback for all jobs whose volume became zero
    for (const auto& entry : calc.getZeroEntries()) {
        if (_local_jobs.count(entry.jobId))
            _volume_update_callback(entry.jobId, 0, 0);
    }
    
    if (rank == 0) LOG(V5_DEBG, "BLC RESULT %s\n", msg.c_str());
    if (_balancing_done_callback) _balancing_done_callback();
}

bool EventDrivenBalancer::hasVolume(int jobId) const {
    return _job_volumes.count(jobId);
}

int EventDrivenBalancer::getVolume(int jobId) const {
    return _job_volumes.at(jobId);
}

int EventDrivenBalancer::getRootRank() {
    return _root_rank;
}
int EventDrivenBalancer::getParentRank() {
    return _parent_rank;
}
const std::vector<int>& EventDrivenBalancer::getChildRanks() {
    return _child_ranks;
}
bool EventDrivenBalancer::isRoot(int rank) {
    return rank == getRootRank();
}
bool EventDrivenBalancer::isLeaf(int rank) {
    return rank % 2 == 1;
}

int EventDrivenBalancer::getNewDemand(int jobId) {
    return _states.getEntries().at(jobId).demand;
}

float EventDrivenBalancer::getPriority(int jobId) {
    return _states.getEntries().at(jobId).priority;
}

size_t EventDrivenBalancer::getGlobalEpoch() const {
    return _states.getGlobalEpoch();
}

EventDrivenBalancer::~EventDrivenBalancer() {

    for (auto& [id, latencies] : _balancing_latencies) {
        if (!latencies.empty())
            _past_balancing_latencies.push_back(std::move(latencies));
    }
    DataStatistics stats(std::move(_past_balancing_latencies));
    stats.computeStats();
    LOG(V3_VERB, "STATS balancing_latencies num:%ld min:%.6f max:%.6f med:%.6f mean:%.6f\n", 
        stats.num(), stats.min(), stats.max(), stats.median(), stats.mean());
    stats.logFullDataIntoFile(".balancing-latencies");
}
