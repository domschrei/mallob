
#include "watchdog.hpp"

#include "util/console.hpp"

Watchdog::Watchdog(long checkIntervMillis, float time) {

    reset(time);

    float maxResetSecs = checkIntervMillis/1000.f;

    _thread = std::thread([&]() {
        while (_running) {
            usleep(1000 * checkIntervMillis);
            auto lock = _reset_lock.getLock();
            if (Timer::elapsedSeconds() - _last_reset > maxResetSecs) {
                Console::log(Console::CRIT, "Watchdog: Timeout detected -- aborting");
                abort();
            }
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