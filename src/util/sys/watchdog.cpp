
#include "watchdog.hpp"

#include <cerrno>
#include <unistd.h>
#include <signal.h>

#include "util/logger.hpp"
#include "util/sys/proc.hpp"
#include "util/sys/process.hpp"
#include "util/sys/thread_pool.hpp"
#include "util/sys/timer.hpp"

Watchdog::Watchdog(bool enabled, int checkIntervalMillis, bool useThreadPool, std::function<void()> customAbortFunction) {
    if (!enabled) return;

    reset(Timer::elapsedSeconds());
    auto parentTid = Proc::getTid();

    _abort_function = [parentTid]() {Process::writeTrace(parentTid);};
    if (customAbortFunction) _abort_function = customAbortFunction;

    auto runnable = [&, useThreadPool, checkIntervalMillis]() {
        if (!useThreadPool) Proc::nameThisThread("Watchdog");
        _worker_pthread_id = Process::getPthreadId();
        _initialized = true;

        while (_worker.continueRunning()) {
            int timeMillis = (int) (1000*Timer::elapsedSeconds());
            auto elapsed = timeMillis - _last_reset_millis;
            if (_globally_enabled.load(std::memory_order_relaxed) && _active) {
                bool doAbort = false;
                if (_abort_period_millis > 0 && elapsed > _abort_period_millis) {
                    doAbort = true;
                } else if (_abort_ticks > 0) {
                    _ticks++;
                    if (_ticks == _abort_ticks) doAbort = true;
                }
                if (doAbort) {
                    LOG(V0_CRIT, "[ERROR] Watchdog: TIMEOUT (last=%.3f activity=%i recvtag=%i sendtag=%i)\n", 
                        0.001*_last_reset_millis, _activity, _activity_recv_tag, _activity_send_tag);
                    _abort_function();
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
                LOG(V5_DEBG, "Watchdog sleep interrupted by signal\n");
            }
        }
    };

    _running = true;
    if (useThreadPool) {
        _fut_thread_pool = ProcessWideThreadPool::get().addTask(std::move(runnable));
    } else {
        _worker.run(std::move(runnable));
    }
}

Watchdog::~Watchdog() {
    if (!_running) return;
    stopWithoutWaiting();
    while (!_initialized) usleep(1000);
    Process::wakeUpThread(_worker_pthread_id);
    if (_fut_thread_pool.valid()) _fut_thread_pool.get();
}

void Watchdog::setWarningPeriod(int periodMillis) {
    _warning_period_millis = periodMillis;
}
void Watchdog::setAbortPeriod(int periodMillis) {
    _abort_period_millis = periodMillis;
}
void Watchdog::setAbortTicks(int nbTicks) {
    _ticks = 0;
    _abort_ticks = nbTicks;
}

void Watchdog::reset(float time) {
    _last_reset_millis = (int) (1000*time);
    _ticks = 0;
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
