
#ifndef DOMPASCH_MALLOB_WATCHDOG_HPP
#define DOMPASCH_MALLOB_WATCHDOG_HPP

#include <thread>
#include <atomic>

#include "timer.hpp"
#include "threading.hpp"

class Watchdog {

private:
    std::thread _thread;
    std::atomic_bool _running = true;

    std::atomic_int _last_reset_millis = 0;
    std::atomic_int _warning_period_millis = 0;
    std::atomic_int _abort_period_millis = 0;

public:
    Watchdog(int checkIntervalMillis, float time = Timer::elapsedSeconds());
    ~Watchdog();
    void setWarningPeriod(int periodMillis);
    void setAbortPeriod(int periodMillis);
    void reset(float time = Timer::elapsedSeconds());
    void stop();

};

#endif