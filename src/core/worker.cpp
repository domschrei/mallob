
#include <cmath>
#include <thread>
#include <unistd.h>
#include <iostream>
#include <fstream>
#include <initializer_list>
#include <limits>

#include "worker.hpp"

#include "balancing/event_driven_balancer.hpp"
#include "data/serializable.hpp"
#include "data/job_description.hpp"
#include "util/sys/process.hpp"
#include "util/sys/proc.hpp"
#include "util/sys/timer.hpp"
#include "util/sys/watchdog.hpp"
#include "util/logger.hpp"
#include "util/random.hpp"
#include "util/sys/terminator.hpp"
#include "util/sys/thread_pool.hpp"
#include "balancing/routing_tree_request_matcher.hpp"
#include "balancing/prefix_sum_request_matcher.hpp"

Worker::Worker(MPI_Comm comm, Parameters& params) :
    _comm(comm), _world_rank(MyMpi::rank(MPI_COMM_WORLD)), 
    _params(params), _job_registry(_params, _comm), _sched_man(_params, _comm, _job_registry, _sys_state), 
    _sys_state(_comm, params.sysstatePeriod(), SysState<9>::ALLREDUCE), 
    _watchdog(/*enabled=*/_params.watchdog(), /*checkIntervMillis=*/100, Timer::elapsedSeconds())
{
    _watchdog.setWarningPeriod(50); // warn after 50ms without a reset
    _watchdog.setAbortPeriod(_params.watchdogAbortMillis()); // abort after X ms without a reset

    _sched_man.setCallbackToDeflectJobRequest([this](JobRequest& req, int source) {
        bounceJobRequest(req, source);
    });
}

void Worker::init() {
    
    // Initialize pseudo-random order of nodes
    auto permutations = createExpanderGraph();

    if (_params.hopsUntilCollectiveAssignment() >= 0) {
        // Create request matcher
        // Callback for receiving a job request
        auto cbReceiveRequest = [&](const JobRequest& req, int rank) {
            MessageHandle handle;
            handle.tag = MSG_REQUEST_NODE;
            handle.finished = true;
            handle.receiveSelfMessage(req.serialize(), rank);
            _sched_man.handleIncomingJobRequest(handle, SchedulingManager::NORMAL);
        };
        if (_params.prefixSumMatching()) {
            // Prefix sum based request matcher
            _req_matcher.reset(new PrefixSumRequestMatcher(_job_registry, _comm, cbReceiveRequest));
        } else {
            // Routing tree based request matcher
            _req_matcher.reset(new RoutingTreeRequestMatcher(
                _job_registry, _comm, 
                AdjustablePermutation::getBestOutgoingEdgeForEachNode(permutations, _world_rank),
                cbReceiveRequest
            ));
        }
        _sched_man.setRequestMatcher(_req_matcher);
    }

    auto& q = MyMpi::getMessageQueue();
    
    // Write tag of currently handled message into watchdog
    q.setCurrentTagPointers(_watchdog.activityRecvTag(), _watchdog.activitySendTag());

    _subscriptions.emplace_back(MSG_WARMUP, [&](auto& h) {
        LOG_ADD_SRC(V4_VVER, "Received warmup msg", h.source);
    });

    // Send warm-up messages with your pseudorandom bounce destinations
    if (_params.warmup()) {
        IntVec payload({1, 2, 3, 4, 5, 6, 7, 8});
        for (auto rank : _hop_destinations) {
            MyMpi::isend(rank, MSG_WARMUP, payload);
            LOG_ADD_DEST(V4_VVER, "Sending warmup msg", rank);
            MyMpi::getMessageQueue().advance();
        }
    }
}

std::vector<std::vector<int>> Worker::createExpanderGraph() {

    // Pick fixed number k of bounce destinations
    int numBounceAlternatives = _params.numBounceAlternatives();
    int numWorkers = MyMpi::size(_comm);
    
    // Check validity of num bounce alternatives
    if (2*numBounceAlternatives > numWorkers) {
        numBounceAlternatives = std::max(1, numWorkers / 2);
        LOG(V1_WARN, "[WARN] Num bounce alternatives must be at most half the number of workers!\n");
        LOG(V1_WARN, "[WARN] Falling back to safe value r=%i.\n", numBounceAlternatives);
    }  

    // Create graph, get outgoing edges from this node
    auto permutations = AdjustablePermutation::getPermutations(numWorkers, numBounceAlternatives);
    _hop_destinations = AdjustablePermutation::createExpanderGraph(permutations, _world_rank);
    
    // Output found bounce alternatives
    std::string info = "";
    for (size_t i = 0; i < _hop_destinations.size(); i++) {
        info += std::to_string(_hop_destinations[i]) + " ";
    }
    LOG(V3_VERB, "My bounce alternatives: %s\n", info.c_str());
    assert((int)_hop_destinations.size() == numBounceAlternatives);

    return permutations;
}

void Worker::advance(float time) {

    // Timestamp provided?
    if (time < 0) time = Timer::elapsedSeconds();

    // Reset watchdog
    _watchdog.reset(time);
    
    // Check & print stats
    if (_periodic_stats_check.ready(time)) {
        _watchdog.setActivity(Watchdog::STATS);
        checkStats(time);
    }

    // Advance load balancing operations
    if (_periodic_balance_check.ready(time)) {
        _watchdog.setActivity(Watchdog::BALANCING);
        _sched_man.advanceBalancing(time);

        // Advance collective assignment of nodes
        if (_params.hopsUntilCollectiveAssignment() >= 0) {
            _watchdog.setActivity(Watchdog::COLLECTIVE_ASSIGNMENT);
            _req_matcher->advance(_sched_man.getGlobalBalancingEpoch());
        }
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

void Worker::checkStats(float time) {

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
    if (_periodic_big_stats_check.ready(time)) {

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

    if (_params.memoryPanic() && _host_comm && _host_comm->advanceAndCheckMemoryPanic(time)) {
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

void Worker::bounceJobRequest(JobRequest& request, int senderRank) {

    // Increment #hops
    request.numHops++;
    int num = request.numHops;
    _sys_state.addLocal(SYSSTATE_NUMHOPS, 1);

    // Show warning if #hops is a large power of two
    if ((num >= 512) && ((num & (num - 1)) == 0)) {
        LOG(V1_WARN, "[WARN] %s\n", request.toStr().c_str());
    }

    // If hopped enough for collective assignment to be enabled
    // and if either reactivation scheduling is employed or the requested node is non-root
    if (_params.hopsUntilCollectiveAssignment() >= 0 && num >= _params.hopsUntilCollectiveAssignment()
        && (_params.reactivationScheduling() || request.requestedNodeIndex > 0)) {
        _req_matcher->addJobRequest(request);
        return;
    }

    // Get random choice from bounce alternatives
    int nextRank = getWeightedRandomNeighbor();
    if (_hop_destinations.size() > 2) {
        // ... if possible while skipping the requesting node and the sender
        while (nextRank == request.requestingNodeRank || nextRank == senderRank) {
            nextRank = getWeightedRandomNeighbor();
        }
    }

    // Send request to "next" worker node
    LOG_ADD_DEST(V5_DEBG, "Hop %s", nextRank, _sched_man.toStr(request.jobId, request.requestedNodeIndex).c_str());
    MyMpi::isend(nextRank, MSG_REQUEST_NODE, request);
}

int Worker::getWeightedRandomNeighbor() {
    int rand = (int) (_hop_destinations.size()*Random::rand());
    return _hop_destinations[rand];
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
