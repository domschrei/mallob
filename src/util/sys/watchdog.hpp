
#ifndef DOMPASCH_MALLOB_WATCHDOG_HPP
#define DOMPASCH_MALLOB_WATCHDOG_HPP

#include <thread>

#include "timer.hpp"
#include "threading.hpp"

class Watchdog {

private:
    std::thread _thread;
    bool _running = true;
    Mutex _reset_lock;
    float _last_reset = 0.0f;

public:
    Watchdog(long checkIntervMillis, float time = Timer::elapsedSeconds());
    ~Watchdog();
    void reset(float time = Timer::elapsedSeconds());
    void stop();

};

#endif