
#include "watchdog.hpp"

#include "util/logger.hpp"

Watchdog::Watchdog(long checkIntervMillis, float time) {

    reset(time);
    float maxResetSecs = ((float)checkIntervMillis)/1000;
    auto parentTid = Proc::getTid();

    _thread = std::thread([&, parentTid, checkIntervMillis, maxResetSecs]() {
        while (_running) {
            usleep(1000 * 1000 /*1 second*/);
            if (!_running) break;
            auto lock = _reset_lock.getLock();
            if (Timer::elapsedSeconds() - _last_reset > maxResetSecs) {
                
                log(V0_CRIT, "Watchdog: Timeout detected! Trying to observe the parent's current location ...\n");
                std::string command = "gdb --q --n --ex bt --batch --pid " + std::to_string(parentTid);
                system(command.c_str());
                log(V0_CRIT, "Aborting.\n");
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