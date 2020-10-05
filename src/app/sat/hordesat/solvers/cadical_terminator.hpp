#include <atomic>
#include <cassert>

#include "app/sat/hordesat/solvers/cadical_interface.hpp"
#include "app/sat/hordesat/utilities/logging_interface.hpp"
#include "util/sys/threading.hpp"

struct HordeTerminator : public CaDiCaL::Terminator {
    HordeTerminator(LoggingInterface &logger) : _logger(logger) {
        _lastTermCallbackTime = logger.getTime();
        _runningMutex = _suspendMutex.getLock();
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

            // Stay inside this function call as long as solver is suspended
            _suspendCond.wait(_runningMutex, [this] { return !_suspend; });
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
    // This method is critical and may only be called when the terminator is suspended
    void unsetSuspend() {
        // Assert that the solver was previously suspended.
        assert(_suspend.load() == true);

        // Wait for _suspendMutex to be aquired. This guarantees that the solver is suspended.
        // The _suspendMutex is aquired during the initialization of this terminator, therefore it happens before any calls to unsetSuspend.
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
    // Holds unique_lock of _suspendMutex during solving. The mutex is unlocked during suspension.
    std::unique_lock<std::mutex> _runningMutex;

    Mutex _unsuspendMutex;

    ConditionVariable _suspendCond;
};
