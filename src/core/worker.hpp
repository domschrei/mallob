
#ifndef DOMPASCH_MALLOB_WORKER_HPP
#define DOMPASCH_MALLOB_WORKER_HPP

#include <set>
#include <chrono>
#include <string>
#include <memory>

#include "comm/mympi.hpp"
#include "util/params.hpp"
#include "app/job.hpp"
#include "data/job_description.hpp"
#include "data/job_result.hpp"
#include "data/job_transfer.hpp"
#include "core/scheduling_manager.hpp"
#include "data/worker_sysstate.hpp"
#include "balancing/request_matcher.hpp"
#include "util/periodic_event.hpp"
#include "util/sys/watchdog.hpp"
#include "comm/host_comm.hpp"
#include "comm/msg_queue/message_subscription.hpp"
#include "comm/randomized_routing_tree.hpp"

/*
Primary actor in the system who is responsible for participating in the scheduling and execution of jobs.
There is at most one Worker instance for each PE.
*/
class Worker {

private:
    MPI_Comm _comm;
    int _world_rank;
    Parameters& _params;

    std::list<MessageSubscription> _subscriptions;

    WorkerSysState _sys_state;
    JobRegistry _job_registry;
    RandomizedRoutingTree _routing_tree;
    SchedulingManager _sched_man;

    long long _iteration = 0;
    PeriodicEvent<1000> _periodic_stats_check;
    PeriodicEvent<2990> _periodic_big_stats_check; // ready at every 3rd "ready" of _periodic_stats_check
    PeriodicEvent<10> _periodic_job_check;
    PeriodicEvent<1> _periodic_balance_check;
    PeriodicEvent<1000> _periodic_maintenance;
    Watchdog _watchdog;

    std::atomic_bool _node_stats_calculated = true;
    float _node_memory_gbs = 0;
    double _mainthread_cpu_share = 0;
    float _mainthread_sys_share = 0;
    unsigned long _machine_free_kbs = 0;
    unsigned long _machine_total_kbs = 0;

    HostComm* _host_comm;

public:
    Worker(MPI_Comm comm, Parameters& params);
    ~Worker();
    void init();
    void advance();
    void setHostComm(HostComm& hostComm) {_host_comm = &hostComm;}

private:
    void checkStats();
    void checkJobs();
    void checkActiveJob();
    void publishAndResetSysState();
};

#endif
