
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
#include "execution/sat_process.hpp"
#include "util/sys/fileutils.hpp"

#ifndef MALLOB_VERSION
#define MALLOB_VERSION "(dbg)"
#endif

int main(int argc, char *argv[]) {
    
    Parameters params;
    params.init(argc, argv);
    SatProcessConfig config(params.satEngineConfig());

    timespec t;
    t.tv_sec = config.starttimeSecs;
    t.tv_nsec = config.starttimeNsecs;
    Timer::init(t);

    int rankOfParent = config.mpirank;

    Random::init(config.mpisize, rankOfParent);

    ProcessWideThreadPool::init(1);

    // Initialize signal handlers
    Process::init(rankOfParent, params.traceDirectory(), /*leafProcess=*/true);

    std::string logdir = params.logDirectory();
    std::string logFilename = "subproc" + std::string(".") + std::to_string(rankOfParent);

    Logger::LoggerConfig logConfig;
    logConfig.rank = rankOfParent;
    logConfig.verbosity = params.verbosity();
    logConfig.coloredOutput = params.coloredOutput();
    logConfig.flushFileImmediately = params.immediateFileFlush();
    logConfig.quiet = params.quiet();
    if (params.zeroOnlyLogging() && rankOfParent > 0) logConfig.quiet = true;
    logConfig.cPrefix = params.monoFilename.isSet();
    logConfig.logDirOrNull = logdir.empty() ? nullptr : &logdir;
    logConfig.logFilenameOrNull = &logFilename;
    Logger::init(logConfig);
    Logger::getMainInstance().setLinePrefix(" <" + config.getJobStr() + ">");
    
    pid_t pid = Proc::getPid();
    LOG(V3_VERB, "Mallob SAT engine %s pid=%lu\n", MALLOB_VERSION, pid);
    
    // Clean up subprocess command tmp file
    FileUtils::rm("/tmp/mallob_subproc_cmd_" + std::to_string(pid));
    
    try {
        // Launch program
        SatProcess p(params, config, Logger::getMainInstance());
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
