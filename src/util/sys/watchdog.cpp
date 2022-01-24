
#include "watchdog.hpp"

#include <unistd.h>

#include "util/logger.hpp"
#include "util/sys/proc.hpp"
#include "util/sys/process.hpp"

Watchdog::Watchdog(int checkIntervalMillis, float time) {

    reset(time);
    auto parentTid = Proc::getTid();

    _worker.run([&, parentTid, checkIntervalMillis]() {
        while (_worker.continueRunning()) {
            int timeMillis = (int) (1000*Timer::elapsedSeconds());
            auto elapsed = timeMillis - _last_reset_millis;
            if (_abort_period_millis > 0 && elapsed > _abort_period_millis) {   
                LOG(V0_CRIT, "[ERROR] Watchdog: Timeout detected! (last=%.3f activity=%i tag=%i) Writing trace ...\n", 
                    0.001*_last_reset_millis, _activity, _activity_tag);
                Process::writeTrace(parentTid);
                Logger::getMainInstance().flush();
                raise(SIGABRT);
            }
            if (_warning_period_millis > 0 && elapsed > _warning_period_millis) {
                LOG(V1_WARN, "[WARN] Watchdog: No reset for %i ms! (activity=%i tag=%i)\n", elapsed, 
                    _activity, _activity_tag);
            }
            usleep(1000 * checkIntervalMillis);
        }
    });
}

void Watchdog::setWarningPeriod(int periodMillis) {
    _warning_period_millis = periodMillis;
}
void Watchdog::setAbortPeriod(int periodMillis) {
    _abort_period_millis = periodMillis;
}

void Watchdog::reset(float time) {
    _last_reset_millis = (int) (1000*time);
}

void Watchdog::stop() {
    _worker.stop();
}
