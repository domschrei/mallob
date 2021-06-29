
#include "watchdog.hpp"

#include "util/logger.hpp"
#include "util/sys/process.hpp"

Watchdog::Watchdog(int checkIntervalMillis, float time) {

    reset(time);
    auto parentTid = Proc::getTid();

    _thread = std::thread([&, parentTid, checkIntervalMillis]() {
        while (_running) {
            usleep(1000 * checkIntervalMillis);
            if (!_running) break;
            int timeMillis = (int) (1000*Timer::elapsedSeconds());
            auto elapsed = timeMillis - _last_reset_millis;
            if (_abort_period_millis > 0 && elapsed > _abort_period_millis) {   
                log(V0_CRIT, "ERROR: Watchdog: Timeout detected! Writing trace ...\n");
                Process::writeTrace(parentTid);
                log(V0_CRIT, "Aborting.\n");
                Logger::getMainInstance().flush();
                abort();
            }
            if (_warning_period_millis > 0 && elapsed > _warning_period_millis) {
                log(V1_WARN, "[WARN] Watchdog: No reset for %i ms!\n", elapsed);
            }
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
    _running = false;
}

Watchdog::~Watchdog() {
    _running = false;
    if (_thread.joinable()) _thread.join();
}