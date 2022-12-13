
#ifndef DOMPASCH_BALANCER_EVENT_DRIVEN_H
#define DOMPASCH_BALANCER_EVENT_DRIVEN_H

#include <utility>
#include <map>
#include <list>
#include <functional>

#include "comm/mympi.hpp"
#include "util/params.hpp"
#include "data/reduceable.hpp"
#include "util/logger.hpp"
#include "balancing/event_map.hpp"
#include "util/periodic_event.hpp"

class Job;

class EventDrivenBalancer {

public:
    EventDrivenBalancer(MPI_Comm& comm, Parameters& params);
    ~EventDrivenBalancer();

    void setVolumeUpdateCallback(std::function<void(int, int, float)> callback);
    void setBalancingDoneCallback(std::function<void()> callback);

    void onProbe(int jobId);
    void onActivate(const Job& job, int demand);
    void onDemandChange(const Job& job, int demand);
    void onSuspend(const Job& job);
    void onTerminate(const Job& job);

    void advance();
    void handle(MessageHandle& handle);

    size_t getGlobalEpoch() const;
    bool hasVolume(int jobId) const;
    int getVolume(int jobId) const;


private:
    const size_t RECENT_BROADCAST_MEMORY = 3;

    MPI_Comm _comm;
    Parameters& _params;

    EventMap _states;
    EventMap _diffs;
    PeriodicEvent<10> _periodic_balancing;
    int _balancing_epoch = 0;

    int _active_job_id = -1;
    robin_hood::unordered_set<int> _local_jobs;
    robin_hood::unordered_map<int, int> _job_root_epochs;
    robin_hood::unordered_map<int, int> _job_volumes;

    robin_hood::unordered_map<int, std::vector<float>> _balancing_latencies;
    std::list<std::vector<float>> _past_balancing_latencies;

    // Maps a job ID to a pair of (time of last balancing event, associated job epoch)
    robin_hood::unordered_map<int, std::pair<int, float>> _pending_entries;

    std::function<void(int, int, float)> _volume_update_callback;
    std::function<void()> _balancing_done_callback;

    int _root_rank;
    int _parent_rank;
    std::vector<int> _child_ranks;

    void pushEvent(const Event& event, bool recordLatency = true);

    void handleData(EventMap& data, int tag, bool checkedReady);
    void reduce(EventMap& data);
    void reduceIfApplicable();
    void broadcast(EventMap& data);
    void digest(const EventMap& data);

    void computeBalancingResult();

    int getRootRank();
    int getParentRank();
    const std::vector<int>& getChildRanks();
    bool isRoot(int rank);
    bool isLeaf(int rank);

    int getNewDemand(int jobId);
    float getPriority(int jobId);
};

#endif