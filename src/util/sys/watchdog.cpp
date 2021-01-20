
#include "watchdog.hpp"

#include "util/console.hpp"

Watchdog::Watchdog(long checkIntervMillis, float time) {

    reset(time);
    float maxResetSecs = ((float)checkIntervMillis)/1000;

    _thread = std::thread([&, checkIntervMillis, maxResetSecs]() {
        while (_running) {
            usleep(1000 * 1000 /*1 second*/);
            if (!_running) break;
            auto lock = _reset_lock.getLock();
            // if (Timer::elapsedSeconds() - _last_reset > maxResetSecs) {
            //     Console::log(Console::CRIT, "Watchdog: Timeout detected -- aborting");
            //     abort();
            // }
        }
    });
}

void Watchdog::reset(float time) {
    auto lock = _reset_lock.getLock();
    _last_reset = time;
}

void Watchdog::stop() {
    _running = false;
}

Watchdog::~Watchdog() {
    _running = false;
    if (_thread.joinable()) _thread.join();
}