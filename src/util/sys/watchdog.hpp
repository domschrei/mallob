
#ifndef DOMPASCH_MALLOB_WATCHDOG_HPP
#define DOMPASCH_MALLOB_WATCHDOG_HPP

#include <atomic>

#include "timer.hpp"
#include "threading.hpp"
#include "util/sys/background_worker.hpp"

class Watchdog {

public:
    enum Activity {
        IDLE_OR_HANDLING_MSG, 
        STATS, 
        BALANCING, 
        COLLECTIVE_ASSIGNMENT, 
        FORGET_OLD_JOBS, 
        THAW_JOB_REQUESTS, 
        CHECK_JOBS, 
        SYSSTATE
    };

private:
    BackgroundWorker _worker;

    Activity _activity;
    int _activity_tag;

    std::atomic_int _last_reset_millis = 0;
    std::atomic_int _warning_period_millis = 0;
    std::atomic_int _abort_period_millis = 0;

public:
    Watchdog(int checkIntervalMillis, float time = Timer::elapsedSeconds());
    void setWarningPeriod(int periodMillis);
    void setAbortPeriod(int periodMillis);
    void reset(float time = Timer::elapsedSeconds());
    inline void setActivity(Activity a) {
        _activity = a;
    }
    int* activityTag() {return &_activity_tag;}
    void stop();
};

#endif