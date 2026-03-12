
#ifndef DOMPASCH_MALLOB_WORKER_HPP
#define DOMPASCH_MALLOB_WORKER_HPP

#include <atomic>
#include <list>

#include "comm/group_comm_builder.hpp"
#include "core/scheduling_manager.hpp"
#include "data/worker_sysstate.hpp"
#include "util/periodic_event.hpp"
#include "util/sys/watchdog.hpp"
#include "comm/msg_queue/message_subscription.hpp"
#include "comm/randomized_routing_tree.hpp"
#include "comm/mpi_base.hpp"
#include "core/job_registry.hpp"

class HostComm;
class Parameters;

/*
Primary actor in the system who is responsible for participating in the scheduling and execution of jobs.
There is at most one Worker instance for each PE.
*/
class Worker {

private:
    MPI_Comm _comm;
    int _world_rank;
    Parameters& _params;

    Watchdog _watchdog;

    std::list<MessageSubscription> _subscriptions;

    WorkerSysState _sys_state;
    JobRegistry _job_registry;
    RandomizedRoutingTree _routing_tree;
    SchedulingManager _sched_man;
    GroupCommBuilder _group_comm_builder;

    long long _iteration = 0;
    PeriodicEvent<1000> _periodic_stats_check;
    PeriodicEvent<2990> _periodic_big_stats_check; // ready at every 3rd "ready" of _periodic_stats_check
    PeriodicEvent<10, 1> _periodic_job_check;
    PeriodicEvent<1> _periodic_balance_check;
    PeriodicEvent<1000> _periodic_maintenance;
    bool _job_active {false};

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
    bool hasJobsLeftToDelete() {
        return _job_active || _sched_man.hasJobsLeftToDelete();
    }

private:
    void checkStats();
    void checkJobs();
    void checkActiveJob();
    void publishAndResetSysState();
};

#endif
