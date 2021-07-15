
#ifndef DOMPASCH_MALLOB_WATCHDOG_HPP
#define DOMPASCH_MALLOB_WATCHDOG_HPP

#include <atomic>

#include "timer.hpp"
#include "threading.hpp"
#include "util/sys/background_worker.hpp"

class Watchdog {

private:
    BackgroundWorker _worker;

    std::atomic_int _last_reset_millis = 0;
    std::atomic_int _warning_period_millis = 0;
    std::atomic_int _abort_period_millis = 0;

public:
    Watchdog(int checkIntervalMillis, float time = Timer::elapsedSeconds());
    void setWarningPeriod(int periodMillis);
    void setAbortPeriod(int periodMillis);
    void reset(float time = Timer::elapsedSeconds());
    void stop();
};

#endif