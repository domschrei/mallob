
#pragma once

#include "util/logger.hpp"
#include "util/sys/threading.hpp"
#include "util/sys/timer.hpp"

#include "cadical/src/cadical.hpp"

struct CadicalTerminator : public CaDiCaL::Terminator {
    CadicalTerminator(Logger &logger) : _logger(logger) {
        _lastTermCallbackTime = Timer::elapsedSeconds();
    };
    ~CadicalTerminator() override {}

    bool terminate() override {

        _is_in_callback = true;
        double time = Timer::elapsedSeconds();
        double elapsed = time - _lastTermCallbackTime;
        _lastTermCallbackTime = time;

        if (_stop) {
            LOGGER(_logger, V4_VVER, "STOP (%.2fs since last cb)\n", elapsed);
            _is_in_callback = false;
            return true;
        }

        if (_suspend) {
            // Stay inside this function call as long as solver is suspended
            LOGGER(_logger, V4_VVER, "SUSPEND (%.2fs since last cb)\n", elapsed);

            _suspendCond.wait(_suspendMutex, [this] { return !_suspend; });
            LOGGER(_logger, V4_VVER, "RESUME\n");

            if (_stop) {
                LOGGER(_logger, V4_VVER, "STOP after suspension\n", elapsed);
                _is_in_callback = false;
                return true;
            }
        }
        _is_in_callback = false;
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
        {
            auto lock = _suspendMutex.getLock();
            _suspend = false;
        }
        _suspendCond.notify();
    }
    bool isThreadInCallback() const {
        return _is_in_callback;
    }

private:
    Logger &_logger;
    double _lastTermCallbackTime;

    int _stop = 0;
    volatile bool _suspend = false;
    volatile bool _is_in_callback = false;

    Mutex _suspendMutex;
    ConditionVariable _suspendCond;
};
