#include <atomic>
#include <cassert>

#include "app/sat/hordesat/solvers/cadical_interface.hpp"
#include "app/sat/hordesat/utilities/logging_interface.hpp"
#include "util/sys/threading.hpp"

struct HordeTerminator : public CaDiCaL::Terminator {
    HordeTerminator(LoggingInterface &logger) : _logger(logger) {
        _lastTermCallbackTime = logger.getTime();
    };

    bool terminate() override {
        double elapsed = _logger.getTime() - _lastTermCallbackTime;
        _lastTermCallbackTime = _logger.getTime();

        if (_interrupt.load()) {
            _logger.log(0, "STOP (%.2fs since last cb)", elapsed);
            return true;
        }

        if (_suspend.load()) {
            _logger.log(0, "SUSPEND (%.2fs since last cb)", elapsed);

            // Wait until the solver is unsuspended
            _suspendCond.wait(_suspendMutex, [this] { return !_suspend; });
            _logger.log(0, "RESUME");

            if (_interrupt.load()) {
                _logger.log(0, "STOP after suspension");
                return true;
            }
        }
        return false;
    }

    void setInterrupt() {
        _interrupt.store(true);
    }
    void unsetInterrupt() {
        _interrupt.store(false);
    }
    void setSuspend() {
        _suspend.store(true);
    }
    void unsetSuspend() {
        const std::lock_guard<Mutex> lock(_suspendMutex);
        _suspend.store(false);
        _suspendCond.notify();
    }

   private:
    LoggingInterface &_logger;
    double _lastTermCallbackTime;

    std::atomic_bool _interrupt{false};
    std::atomic_bool _suspend{false};

    Mutex _suspendMutex;
    ConditionVariable _suspendCond;
};
