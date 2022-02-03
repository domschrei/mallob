
#ifndef DOMPASCH_MALLOB_TERMINATOR_HPP
#define DOMPASCH_MALLOB_TERMINATOR_HPP

#include <atomic>
#include <signal.h>

#include "util/logger.hpp"
#include "util/sys/process.hpp"
#include "util/sys/proc.hpp"

class Terminator {

private:
    static std::atomic_bool _exit;

public:
    static void setTerminating() {
        _exit = true;
    }
    static inline bool isTerminating(bool fromMainThread = false) {
        
        if (Process::wasSignalCaught()) {

            auto optSignalInfo = Process::getCaughtSignal();
            if (optSignalInfo) {

                int signum = optSignalInfo.value().signum;
                if (!_exit) LOG(V2_INFO, "Caught signal %i\n", signum);
                setTerminating();

                if (fromMainThread) {
                    Process::handleTerminationSignal(optSignalInfo.value());
                }

                return true;
            }
        }

        return _exit.load(std::memory_order_relaxed);
    }
    static void reset() {
        _exit = false;
    }

};

#endif