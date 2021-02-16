
#include "watchdog.hpp"

#include "util/logger.hpp"
#include "util/sys/process.hpp"

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
                
                log(V0_CRIT, "Watchdog: Timeout detected! Writing trace ...\n");
                Process::writeTrace(parentTid);
                log(V0_CRIT, "Aborting.\n");
                Logger::getMainInstance().flush();
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