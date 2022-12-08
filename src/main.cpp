
#include <iostream>
#include <set>
#include <stdlib.h>
#include <unistd.h>
#include <algorithm> 
#include <string>

#include "comm/mympi.hpp"
#include "util/sys/timer.hpp"
#include "util/logger.hpp"
#include "util/random.hpp"
#include "util/params.hpp"
#include "util/sys/shared_memory.hpp"
#include "util/sys/process.hpp"
#include "util/sys/proc.hpp"
#include "core/worker.hpp"
#include "core/client.hpp"
#include "util/sys/thread_pool.hpp"
#include "interface/api/job_streamer.hpp"
#include "comm/host_comm.hpp"
#include "data/job_transfer.hpp"
#include "comm/msg_queue/message_subscription.hpp"

#include "app/register_includes.h"

#ifndef MALLOB_VERSION
#define MALLOB_VERSION "(dbg)"
#endif

bool monoJobDone = false;
void introduceMonoJob(Parameters& params, Client& client) {

    // Parse application name
    auto app = params.monoApplication();
    std::transform(app.begin(), app.end(), app.begin(), ::toupper);
    LOG(V2_INFO, "Assuming application \"%s\" for mono job\n", app.c_str());

    // Write a job JSON for the singular job to solve
    nlohmann::json json = {
        {"user", "admin"},
        {"name", "mono-job"},
        {"files", {params.monoFilename()}},
        {"priority", 1.000},
        {"application", app}
    };
    if (params.jobWallclockLimit() > 0)
        json["wallclock-limit"] = std::to_string(params.jobWallclockLimit()) + "s";
    if (params.jobCpuLimit() > 0) {
        json["cpu-limit"] = std::to_string(params.jobCpuLimit()) + "s";
    }

    auto result = client.getAPI().submit(json, [&](nlohmann::json& response) {
        // Job done? => Terminate all processes
        monoJobDone = true;
    });
    if (result != JsonInterface::Result::ACCEPT) {
        LOG(V0_CRIT, "[ERROR] Cannot introduce mono job!\n");
        abort();
    }
}

inline bool doTerminate(Parameters& params, int rank) {
    
    bool terminate = false;
    if (Terminator::isTerminating(/*fromMainThread=*/true)) terminate = true;
    if (params.timeLimit() > 0 && Timer::elapsedSecondsCached() > params.timeLimit()) {
        terminate = true;
    }
    if (terminate) {
        if (rank == 0) {
            LOG(V2_INFO, "Terminating.\n");
        } else {
            LOG(V3_VERB, "Terminating.\n");
        }
        Terminator::setTerminating();
        return true;
    }
    return false;
}

void doMainProgram(MPI_Comm& commWorkers, MPI_Comm& commClients, Parameters& params) {

    // Determine which role(s) this PE has
    bool isWorker = commWorkers != MPI_COMM_NULL;
    bool isClient = commClients != MPI_COMM_NULL;
    if (isWorker) LOG(V4_VVER, "I am worker #%i\n", MyMpi::rank(commWorkers));
    if (isClient) LOG(V4_VVER, "I am client #%i\n", MyMpi::rank(commClients));

    // Create worker and client as necessary
    Worker* worker = isWorker ? new Worker(commWorkers, params) : nullptr;
    Client* client = isClient ? new Client(commClients, params) : nullptr;
    
    // Initialize worker and client as necessary (background threads, callbacks, ...)
    if (isWorker) worker->init();
    if (isClient) client->init();
    int myRank = MyMpi::rank(MPI_COMM_WORLD);
    
    // Register global callback for exiting msg (not specific to worker nor client)
    MessageSubscription exitSubscription(MSG_DO_EXIT, [myRank](MessageHandle& h) {
        LOG_ADD_SRC(V3_VERB, "Received exit signal", h.source);

        // Forward exit signal
        if (myRank*2+1 < MyMpi::size(MPI_COMM_WORLD))
            MyMpi::isendCopy(myRank*2+1, MSG_DO_EXIT, h.getRecvData());
        if (myRank*2+2 < MyMpi::size(MPI_COMM_WORLD))
            MyMpi::isendCopy(myRank*2+2, MSG_DO_EXIT, h.getRecvData());

        Terminator::setTerminating();
    });

    // Deposit information to coordinate the creation of an intra-machine communicator
    HostComm hostComm(commWorkers, params);
    hostComm.depositInformation();

    LOG(V5_DEBG, "Global init barrier ...\n");
    MPI_Barrier(MPI_COMM_WORLD);
    LOG(V5_DEBG, "Passed global init barrier\n");

    // Create intra-machine communicator (collective operation)
    hostComm.create();
    if (isWorker) worker->setHostComm(hostComm);

    // If mono solving mode is enabled, introduce the singular job to solve
    if (params.monoFilename.isSet() && isClient && MyMpi::rank(commClients) == 0)
        introduceMonoJob(params, *client);

    // If job streaming is enabled, initialize a corresponding job streamer
    JobStreamer* streamer = nullptr;
    if (params.jobTemplate.isSet() && isClient) {
        streamer = new JobStreamer(params, client->getAPI(), client->getInternalRank());
    }

    // Main loop
    while (true) {

        // update cached timing
        Timer::cacheElapsedSeconds();

        // Advance worker and client logic
        if (isWorker) worker->advance();
        if (isClient) client->advance();

        // Advance message queue and run callbacks for done messages
        MyMpi::getMessageQueue().advance();

        // Check termination, sleep, and/or yield thread
        if (doTerminate(params, myRank)) 
            break;
        if (params.sleepMicrosecs() > 0) usleep(params.sleepMicrosecs());
        if (params.yield()) std::this_thread::yield();
        if (monoJobDone) {
            // Terminate all processes
            MyMpi::isend(0, MSG_DO_EXIT, IntVec({0}));
        }
    }

    // Clean up
    if (streamer != nullptr) delete streamer;
    if (isWorker) delete worker;
    if (isClient) delete client;
}

void longStartupWarnMsg(int rank, const char* msg) {
    if (Timer::elapsedSeconds() >= 10) 
        std::cout << Timer::elapsedSeconds() << " " << rank << " " << std::string(msg) << std::endl;
}

int main(int argc, char *argv[]) {
    
    MyMpi::init();
    Timer::init();
    Proc::nameThisThread("MainThread");

    int numNodes = MyMpi::size(MPI_COMM_WORLD);
    int rank = MyMpi::rank(MPI_COMM_WORLD);

    longStartupWarnMsg(rank, "Init'd MPI");

    Parameters params;
    params.init(argc, argv);
    if (rank == 0) params.printBanner();

    longStartupWarnMsg(rank, "Init'd params");

    // Initialize bookkeeping of child processes and signals
    Process::init(rank, params.traceDirectory());

    longStartupWarnMsg(rank, "Init'd process");

    Logger::LoggerConfig logConfig;
    logConfig.rank = rank;
    logConfig.verbosity = params.verbosity();
    logConfig.coloredOutput = params.coloredOutput();
    logConfig.flushFileImmediately = params.immediateFileFlush();
    logConfig.quiet = params.quiet();
    if (params.zeroOnlyLogging() && rank > 0) logConfig.quiet = true;
    logConfig.cPrefix = params.monoFilename.isSet();
    std::string logDirectory = params.logDirectory();
    std::string logFilename = "log." + std::to_string(rank);
    logConfig.logDirOrNull = logDirectory.empty() ? nullptr : &logDirectory;
    logConfig.logFilenameOrNull = &logFilename;
    Logger::init(logConfig);

    longStartupWarnMsg(rank, "Init'd logger");

    MyMpi::setOptions(params);

    longStartupWarnMsg(rank, "Init'd message queue");

    // Register all applications which were compiled into Mallob
    #include "app/register_commands.h"

    if (rank == 0)
        LOG(V2_INFO, "Program options: %s\n", params.getParamsAsString().c_str());
    if (params.help()) {
        // Help requested or no job input provided
        if (rank == 0) {
            params.printUsage();
        }
        MPI_Finalize();
        Process::doExit(0);
    }

    char hostname[1024];
	gethostname(hostname, 1024);
    LOG(V3_VERB, "Mallob %s pid=%lu on host %s\n", MALLOB_VERSION, Proc::getPid(), hostname);

    // Global and local seed, such that all nodes have access to a synchronized randomness
    // as well as to an individual randomness that differs among nodes
    Random::init(numNodes+params.seed(), rank+params.seed());

    auto isWorker = [&](int rank) {
        if (params.numWorkers() == -1) return true; 
        return rank < params.numWorkers();
    };
    auto isClient = [&](int rank) {
        if (params.monoFilename.isSet()) return rank == 0;
        if (params.numClients() == -1) return true;
        return rank >= numNodes - params.numClients();
    };

    // Create communicators for clients and for workers
    std::vector<int> clientRanks;
    std::vector<int> workerRanks;
    for (int i = 0; i < numNodes; i++) {
        if (isWorker(i)) workerRanks.push_back(i);
        if (isClient(i)) clientRanks.push_back(i);
    }
    if (rank == 0) LOG(V3_VERB, "%i workers, %i clients\n", workerRanks.size(), clientRanks.size());

    // Initialize thread pool
    int threadPoolSize = std::max(4, 2*params.numThreadsPerProcess());
    if (isClient(rank) && isWorker(rank)) threadPoolSize *= 2;
    ProcessWideThreadPool::init(threadPoolSize);
    
    MPI_Comm clientComm, workerComm;
    {
        MPI_Group worldGroup;
        MPI_Comm_group(MPI_COMM_WORLD, &worldGroup);
        MPI_Group clientGroup;
        MPI_Group_incl(worldGroup, clientRanks.size(), clientRanks.data(), &clientGroup);
        MPI_Comm_create(MPI_COMM_WORLD, clientGroup, &clientComm);
    }
    if (rank == 0) LOG(V3_VERB, "Created client communicator\n");
    {
        MPI_Group worldGroup;
        MPI_Comm_group(MPI_COMM_WORLD, &worldGroup);
        MPI_Group workerGroup;
        MPI_Group_incl(worldGroup, workerRanks.size(), workerRanks.data(), &workerGroup);
        MPI_Comm_create(MPI_COMM_WORLD, workerGroup, &workerComm);
    }
    if (rank == 0) LOG(V3_VERB, "Created worker communicator\n");
    
    // Execute main program
    try {
        doMainProgram(workerComm, clientComm, params);
    } catch (const std::exception& ex) {
        LOG(V0_CRIT, "[ERROR] uncaught \"%s\"\n", ex.what());
        Process::doExit(1);
    } catch (...) {
        LOG(V0_CRIT, "[ERROR] uncaught exception\n");
        Process::doExit(1);
    }

    // Exit properly
    MPI_Barrier(MPI_COMM_WORLD);
    MPI_Finalize();
    LOG(V2_INFO, "Exiting happily\n");
    Process::doExit(0);
}
