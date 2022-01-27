
#include <iostream>
#include <stdlib.h>
#include <unistd.h>
#include <string>
#include <vector>
#include <memory>
#include "util/assert.hpp"

#include "util/sys/timer.hpp"
#include "util/logger.hpp"
#include "util/params.hpp"
#include "util/sys/shared_memory.hpp"
#include "util/sys/process.hpp"
#include "util/sys/proc.hpp"
#include "util/sys/thread_pool.hpp"
#include "data/checksum.hpp"
#include "app/sat/horde_process.hpp"

#include "app/sat/horde_process_adapter.hpp"
#include "hordesat/horde.hpp"

#ifndef MALLOB_VERSION
#define MALLOB_VERSION "(dbg)"
#endif

int main(int argc, char *argv[]) {
    
    Parameters params;
    params.init(argc, argv);
    HordeConfig config(params.hordeConfig());

    timespec t;
    t.tv_sec = config.starttimeSecs;
    t.tv_nsec = config.starttimeNsecs;
    Timer::init(t);

    int rankOfParent = config.mpirank;

    Random::init(config.mpisize, rankOfParent);

    ProcessWideThreadPool::init(1);

    // Initialize signal handlers
    Process::init(rankOfParent, /*leafProcess=*/true);

    std::string logdir = params.logDirectory();
    std::string logFilename = "subproc" + std::string(".") + std::to_string(rankOfParent);
    Logger::init(rankOfParent, params.verbosity(), params.coloredOutput(), 
            params.quiet(), /*cPrefix=*/params.monoFilename.isSet(),
            !logdir.empty() ? &logdir : nullptr,
            &logFilename);
    Logger::getMainInstance().setLinePrefix(" <" + config.getJobStr() + ">");
    
    pid_t pid = Proc::getPid();
    LOG(V3_VERB, "Mallob SAT engine %s pid=%lu\n", MALLOB_VERSION, pid);
    
    try {
        // Launch program
        HordeProcess p(params, config, Logger::getMainInstance());
        p.run(); // does not return

    } catch (const std::exception &ex) {
        LOG(V0_CRIT, "[ERROR] uncaught \"%s\"\n", ex.what());
        Logger::getMainInstance().flush();
        Process::doExit(1);
    } catch (...) {
        LOG(V0_CRIT, "[ERROR] uncaught exception\n");
        Logger::getMainInstance().flush();
        Process::doExit(1);
    }
}
