
#include "watchdog.hpp"

#include <cerrno>
#include <unistd.h>
#include <signal.h>

#include "util/logger.hpp"
#include "util/sys/proc.hpp"
#include "util/sys/process.hpp"
#include "util/sys/timer.hpp"

Watchdog::Watchdog(bool enabled, int checkIntervalMillis, float time) {
    if (!enabled) return;

    reset(time);
    auto parentTid = Proc::getTid();

    _worker.run([&, parentTid, checkIntervalMillis]() {
        Proc::nameThisThread("Watchdog");
        _worker_pthread_id = Process::getPthreadId();

        while (_worker.continueRunning()) {
            int timeMillis = (int) (1000*Timer::elapsedSeconds());
            auto elapsed = timeMillis - _last_reset_millis;
            if (_globally_enabled.load(std::memory_order_relaxed) && _active) {
                if (_abort_period_millis > 0 && elapsed > _abort_period_millis) {   
                    LOG(V0_CRIT, "[ERROR] Watchdog: TIMEOUT (last=%.3f activity=%i recvtag=%i sendtag=%i)\n", 
                        0.001*_last_reset_millis, _activity, _activity_recv_tag, _activity_send_tag);
                    Process::writeTrace(parentTid);
                    Logger::getMainInstance().flush();
                    abort();
                }
                if (_warning_period_millis > 0 && elapsed > _warning_period_millis) {
                    LOG(V1_WARN, "[WARN] Watchdog: No reset for %i ms (activity=%i recvtag=%i sendtag=%i)\n", 
                        elapsed, _activity, _activity_recv_tag, _activity_send_tag);
                }
            }
            int res = usleep(1000 * checkIntervalMillis);
            if (res == -1 && errno == EINTR) {
                LOG(V4_VVER, "Watchdog sleep interrupted by signal\n");
            }
        }
    });
}

Watchdog::~Watchdog() {
    stopWithoutWaiting();
    if (_worker_pthread_id != 0)
        Process::wakeUpThread(_worker_pthread_id);
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

void Watchdog::stopWithoutWaiting() {
    _worker.stopWithoutWaiting();
}

std::atomic_bool Watchdog::_globally_enabled {true};
void Watchdog::disableGlobally() {
    _globally_enabled.store(false, std::memory_order_relaxed);
}
