
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

Logger getLog(const Parameters& params, const HordeConfig& config) {
    return Logger::getMainInstance().copy("<" + config.getJobStr() + ">", "#" + std::to_string(config.jobid) + ".");
}

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
    Logger::init(rankOfParent, params.verbosity(), params.coloredOutput(), 
            params.quiet(), /*cPrefix=*/params.monoFilename.isSet(),
            !logdir.empty() ? &logdir : nullptr);
    
    auto log = getLog(params, config);
    pid_t pid = Proc::getPid();
    LOGGER(log, V3_VERB, "Mallob SAT engine %s pid=%lu\n", MALLOB_VERSION, pid);
    
    try {
        // Launch program
        HordeProcess p(params, config, log);
        p.run(); // does not return

    } catch (const std::exception &ex) {
        LOGGER(log, V0_CRIT, "[ERROR] uncaught \"%s\"\n", ex.what());
        log.flush();
        Process::doExit(1);
    } catch (...) {
        LOGGER(log, V0_CRIT, "[ERROR] uncaught exception\n");
        log.flush();
        Process::doExit(1);
    }
}
