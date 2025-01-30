
#ifndef DOMPASCH_MALLOB_WATCHDOG_HPP
#define DOMPASCH_MALLOB_WATCHDOG_HPP

#include <atomic>
#include <future>

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
    Activity _activity = Activity::IDLE_OR_HANDLING_MSG;
    int _activity_recv_tag = 0;
    int _activity_send_tag = 0;

    std::atomic_int _last_reset_millis = 0;
    std::atomic_int _warning_period_millis = 0;
    std::atomic_int _abort_period_millis = 0;

    static std::atomic_bool _globally_enabled;
    bool _active {true};

    BackgroundWorker _worker;
    std::future<void> _fut_thread_pool;
    pthread_t _worker_pthread_id {0};

    std::function<void()> _abort_function;

public:
    Watchdog(bool enabled, int checkIntervalMillis, bool useThreadPool = false,
        std::function<void()> customAbortFunction = std::function<void()>());
    ~Watchdog();
    void setWarningPeriod(int periodMillis);
    void setAbortPeriod(int periodMillis);
    void reset(float time = Timer::elapsedSeconds());
    void setActive(bool active) {_active = active;}
    inline void setActivity(Activity a) {
        _activity = a;
    }
    int* activityRecvTag() {return &_activity_recv_tag;}
    int* activitySendTag() {return &_activity_send_tag;}
    void stop();
    void stopWithoutWaiting();

    static void disableGlobally();
};

#endif