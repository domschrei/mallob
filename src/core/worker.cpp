
#include <cmath>
#include <thread>
#include <unistd.h>
#include <iostream>
#include <fstream>
#include <initializer_list>
#include <limits>

#include "worker.hpp"

#include "util/sys/process.hpp"
#include "util/sys/proc.hpp"
#include "util/sys/timer.hpp"
#include "util/sys/watchdog.hpp"
#include "util/logger.hpp"
#include "util/sys/terminator.hpp"
#include "util/sys/thread_pool.hpp"
#include "comm/message_warmup.hpp"

Worker::Worker(MPI_Comm comm, Parameters& params) :
    _comm(comm), _world_rank(MyMpi::rank(MPI_COMM_WORLD)), 
    _params(params), _job_registry(_params, _comm), _routing_tree(_params, _comm), 
    _sys_state(_comm, params.sysstatePeriod(), SysState<9>::ALLREDUCE), 
    _sched_man(_params, _comm, _routing_tree, _job_registry, _sys_state), 
    _watchdog(/*enabled=*/_params.watchdog(), /*checkIntervMillis=*/100, Timer::elapsedSeconds())
{
    _watchdog.setWarningPeriod(50); // warn after 50ms without a reset
    _watchdog.setAbortPeriod(_params.watchdogAbortMillis()); // abort after X ms without a reset
}

void Worker::init() {
    
    // Write tag of currently handled message into watchdog
    MyMpi::getMessageQueue().setCurrentTagPointers(_watchdog.activityRecvTag(), _watchdog.activitySendTag());

    // Send warm-up messages
    if (_params.warmup()) {
        MessageWarmup(_comm, _routing_tree.getNeighbors()).performWarmup();
    }
}

void Worker::advance() {

    auto time = Timer::elapsedSecondsCached();

    // Reset watchdog
    _watchdog.reset(time);
    
    // Check & print stats
    if (_periodic_stats_check.ready(time)) {
        _watchdog.setActivity(Watchdog::STATS);
        checkStats();
    }

    // Advance load balancing operations
    if (_periodic_balance_check.ready(time)) {
        _watchdog.setActivity(Watchdog::BALANCING);
        _sched_man.advanceBalancing();
    }

    // Do diverse periodic maintenance tasks
    if (_periodic_maintenance.ready(time)) {
        
        // Forget jobs that are old or wasting memory
        _watchdog.setActivity(Watchdog::FORGET_OLD_JOBS);
        _sched_man.forgetOldJobs();

        // Continue to bounce requests which were deferred earlier
        _watchdog.setActivity(Watchdog::THAW_JOB_REQUESTS);
        _sched_man.forwardDeferredRequests();
    }

    // Check jobs
    if (_periodic_job_check.ready(time)) {
        _watchdog.setActivity(Watchdog::CHECK_JOBS);
        checkJobs();
    }

    // Advance an all-reduction of the current system state
    if (_sys_state.aggregate(time)) {
        _watchdog.setActivity(Watchdog::SYSSTATE);
        publishAndResetSysState();
    }

    _watchdog.setActivity(Watchdog::IDLE_OR_HANDLING_MSG);
}

void Worker::checkStats() {

    // For this process and subprocesses
    if (_node_stats_calculated.load(std::memory_order_acquire)) {
        
        // Update local sysstate, log update
        _sys_state.setLocal(SYSSTATE_GLOBALMEM, _node_memory_gbs);
        LOG(V4_VVER, "mem=%.2fGB mt_cpu=%.3f mt_sys=%.3f\n", _node_memory_gbs, _mainthread_cpu_share, _mainthread_sys_share);

        // Update host-internal communicator
        if (_host_comm) {
            _host_comm->setRamUsageThisWorkerGbs(_node_memory_gbs);
            _host_comm->setFreeAndTotalMachineMemoryKbs(_machine_free_kbs, _machine_total_kbs);
            if (_job_registry.hasActiveJob()) {
                _host_comm->setActiveJobIndex(_job_registry.getActive().getIndex());
            } else {
                _host_comm->unsetActiveJobIndex();
            }
        }

        // Recompute stats for next query time
        // (concurrently because computation of PSS is expensive)
        _node_stats_calculated.store(false, std::memory_order_relaxed);
        auto tid = Proc::getTid();
        ProcessWideThreadPool::get().addTask([&, tid]() {
            auto memoryKbs = Proc::getRecursiveProportionalSetSizeKbs(Proc::getPid());
            auto memoryGbs = memoryKbs / 1024.f / 1024.f;
            _node_memory_gbs = memoryGbs;
            Proc::getThreadCpuRatio(tid, _mainthread_cpu_share, _mainthread_sys_share);
            auto [freeKbs, totalKbs] = Proc::getMachineFreeAndTotalRamKbs();
            _machine_free_kbs = freeKbs;
            _machine_total_kbs = totalKbs;
            _node_stats_calculated.store(true, std::memory_order_release);
        });
    }

    // Print further stats?
    if (_periodic_big_stats_check.ready(Timer::elapsedSecondsCached())) {

        // For the current job
        if (_job_registry.hasActiveJob()) {
            Job& job = _job_registry.getActive();
            job.appl_dumpStats();
            if (job.getJobTree().isRoot()) {
                std::string commStr = "";
                for (size_t i = 0; i < job.getJobComm().size(); i++) {
                    commStr += " " + std::to_string(job.getJobComm()[i]);
                }
                if (!commStr.empty()) LOG(V4_VVER, "%s job comm:%s\n", job.toStr(), commStr.c_str());
            }
        }
    }

    if (_params.memoryPanic() && _host_comm && 
            _host_comm->advanceAndCheckMemoryPanic(Timer::elapsedSecondsCached())) {
        _sched_man.triggerMemoryPanic();
    }
}

void Worker::checkJobs() {

    // Load and try to adopt pending root reactivation request
    _sched_man.tryAdoptPendingRootActivationRequest();

    if (!_job_registry.hasActiveJob()) {
        if (_job_registry.isBusyOrCommitted()) {
            // PE is committed but not active
            _sys_state.setLocal(SYSSTATE_BUSYRATIO, 1.0f); // busy nodes
            _sys_state.setLocal(SYSSTATE_COMMITTEDRATIO, 1.0f); // committed nodes
        } else {
            // PE is completely idle
            _sys_state.setLocal(SYSSTATE_BUSYRATIO, 0.0f); // busy nodes
            _sys_state.setLocal(SYSSTATE_COMMITTEDRATIO, 0.0f); // committed nodes
        }
        _sys_state.setLocal(SYSSTATE_NUMJOBS, 0.0f); // active jobs
    } else {
        checkActiveJob();
    }
}

void Worker::checkActiveJob() {
    
    // PE runs an active job
    Job &job = _job_registry.getActive();
    int id = job.getId();
    bool isRoot = job.getJobTree().isRoot();

    _sys_state.setLocal(SYSSTATE_BUSYRATIO, 1.0f); // busy nodes
    _sys_state.setLocal(SYSSTATE_COMMITTEDRATIO, 0.0f); // committed nodes
    _sys_state.setLocal(SYSSTATE_NUMJOBS, isRoot ? 1.0f : 0.0f); // active jobs

    _sched_man.checkActiveJob();
}

void Worker::publishAndResetSysState() {

    if (_world_rank == 0) {
        const auto& result = _sys_state.getGlobal();
        int numDesires = result[SYSSTATE_NUMDESIRES];
        int numFulfilledDesires = result[SYSSTATE_NUMFULFILLEDDESIRES];
        float ratioFulfilled = numDesires <= 0 ? 0 : (float)numFulfilledDesires / numDesires;
        float latency = numFulfilledDesires <= 0 ? 0 : result[SYSSTATE_SUMDESIRELATENCIES] / numFulfilledDesires;

        LOG(V2_INFO, "sysstate busyratio=%.3f cmtdratio=%.3f jobs=%i globmem=%.2fGB newreqs=%i hops=%i\n", 
                    result[SYSSTATE_BUSYRATIO]/MyMpi::size(_comm), result[SYSSTATE_COMMITTEDRATIO]/MyMpi::size(_comm), 
                    (int)result[SYSSTATE_NUMJOBS], result[SYSSTATE_GLOBALMEM], (int)result[SYSSTATE_SPAWNEDREQUESTS], 
                    (int)result[SYSSTATE_NUMHOPS]);
    }
    
    if (!_job_registry.isBusyOrCommitted()) {
        LOG(V4_VVER, "I am idle\n");
    }

    // Reset fields which are added to incrementally
    _sys_state.setLocal(SYSSTATE_NUMHOPS, 0);
    _sys_state.setLocal(SYSSTATE_SPAWNEDREQUESTS, 0);
    _sys_state.setLocal(SYSSTATE_NUMDESIRES, 0);
    _sys_state.setLocal(SYSSTATE_NUMFULFILLEDDESIRES, 0);
    _sys_state.setLocal(SYSSTATE_SUMDESIRELATENCIES, 0);
}

Worker::~Worker() {

    _watchdog.stop();
    Terminator::setTerminating();

    LOG(V4_VVER, "Destruct worker\n");

    if (_params.monoFilename.isSet() && _params.applicationSpawnMode() != "fork") {
        // Terminate directly without destructing resident job
        MPI_Finalize();
        Process::doExit(0);
    }
}
