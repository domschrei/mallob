#include "app/sat/hordesat/utilities/logging_interface.hpp"
#include "util/sys/threading.hpp"

#include "app/sat/hordesat/cadical/cadical.hpp"
#include "app/sat/hordesat/solvers/cadical.hpp"

struct HordeTerminator : public CaDiCaL::Terminator {
    HordeTerminator(LoggingInterface &logger) : _logger(logger) {
        _lastTermCallbackTime = logger.getTime();
    };
    ~HordeTerminator() override {}

    bool terminate() override {
        double elapsed = _logger.getTime() - _lastTermCallbackTime;
        _lastTermCallbackTime = _logger.getTime();

        if (_stop) {
            _logger.log(1, "STOP (%.2fs since last cb)", elapsed);
            return true;
        }

        if (_suspend) {
            // Stay inside this function call as long as solver is suspended
            _logger.log(1, "SUSPEND (%.2fs since last cb)", elapsed);

            _suspendCond.wait(_suspendMutex, [this] { return !_suspend; });
            _logger.log(2, "RESUME");

            if (_stop) {
                _logger.log(2, "STOP after suspension", elapsed);
                return true;
            }
        }
        return false;
    }

    void setInterrupt() {
        _stop = 1;
    }
    void unsetInterrupt() {
        _stop = 0;
    }
    void setSuspend() {
        _suspend = true;
    }
    void unsetSuspend() {
        _suspend = false;
        _suspendCond.notify();
    }

private:
    LoggingInterface &_logger;
    double _lastTermCallbackTime;

    int _stop = 0;
    volatile bool _suspend = false;

    Mutex _suspendMutex;
    ConditionVariable _suspendCond;
};
