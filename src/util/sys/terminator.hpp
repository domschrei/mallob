
#ifndef DOMPASCH_MALLOB_TERMINATOR_HPP
#define DOMPASCH_MALLOB_TERMINATOR_HPP

#include <signal.h>
#include <atomic>
#include <optional>

#include "util/logger.hpp"
#include "util/sys/process.hpp"
#include "util/sys/proc.hpp"

class Terminator {

private:
    static std::atomic_bool _exit;

public:
    static void setTerminating() {
        bool expected = false;
        bool isExitNew = _exit.compare_exchange_strong(expected, true, std::memory_order_relaxed);
        if (isExitNew) {
            Process::forwardTerminateToChildren();
        }
    }
    static inline bool isTerminating(bool fromMainThread = false) {
        
        if (!_exit && Process::wasSignalCaught()) {
            auto optSignalInfo = Process::getCaughtSignal();
            if (optSignalInfo) {
                int signum = optSignalInfo.value().signum;
                if (fromMainThread) {
                    Process::reportTerminationSignal(optSignalInfo.value());
                }
                setTerminating();
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