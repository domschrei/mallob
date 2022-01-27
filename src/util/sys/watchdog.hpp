
#ifndef DOMPASCH_MALLOB_WATCHDOG_HPP
#define DOMPASCH_MALLOB_WATCHDOG_HPP

#include <atomic>

#include "timer.hpp"
#include "threading.hpp"
#include "util/sys/background_worker.hpp"

class Watchdog {

public:
    enum Activity {
        /*0*/ IDLE_OR_HANDLING_MSG, 
        /*1*/ STATS, 
        /*2*/ BALANCING, 
        /*3*/ COLLECTIVE_ASSIGNMENT, 
        /*4*/ FORGET_OLD_JOBS, 
        /*5*/ THAW_JOB_REQUESTS, 
        /*6*/ CHECK_JOBS, 
        /*7*/ SYSSTATE
    };

private:
    BackgroundWorker _worker;

    Activity _activity = Activity::IDLE_OR_HANDLING_MSG;
    int _activity_recv_tag = 0;
    int _activity_send_tag = 0;

    std::atomic_int _last_reset_millis = 0;
    std::atomic_int _warning_period_millis = 0;
    std::atomic_int _abort_period_millis = 0;

public:
    Watchdog(bool enabled, int checkIntervalMillis, float time = Timer::elapsedSeconds());
    void setWarningPeriod(int periodMillis);
    void setAbortPeriod(int periodMillis);
    void reset(float time = Timer::elapsedSeconds());
    inline void setActivity(Activity a) {
        _activity = a;
    }
    int* activityRecvTag() {return &_activity_recv_tag;}
    int* activitySendTag() {return &_activity_send_tag;}
    void stop();
};

#endif