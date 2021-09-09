
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
    ~EventDrivenBalancer() {}

    void setVolumeUpdateCallback(std::function<void(int, int)> callback);

    void onActivate(const Job& job);
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
    robin_hood::unordered_map<int, int> _job_root_epochs;
    robin_hood::unordered_map<int, int> _job_volumes;

    std::function<void(int, int)> _volume_update_callback;

    int _root_rank;
    int _parent_rank;
    std::vector<int> _child_ranks;

    void pushEvent(const Event& event);

    void handleData(EventMap& data, int tag);
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