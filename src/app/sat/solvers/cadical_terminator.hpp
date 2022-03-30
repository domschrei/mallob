
#pragma once

#include "util/logger.hpp"
#include "util/sys/threading.hpp"
#include "util/sys/timer.hpp"

#include "cadical_interface.hpp"

struct HordeTerminator : public CaDiCaL::Terminator {
    HordeTerminator(Logger &logger) : _logger(logger) {
        _lastTermCallbackTime = Timer::elapsedSeconds();
    };
    ~HordeTerminator() override {}

    bool terminate() override {
        double time = Timer::elapsedSeconds();
        double elapsed = time - _lastTermCallbackTime;
        _lastTermCallbackTime = time;

        if (_stop) {
            LOGGER(_logger, V3_VERB, "STOP (%.2fs since last cb)\n", elapsed);
            return true;
        }

        if (_suspend) {
            // Stay inside this function call as long as solver is suspended
            LOGGER(_logger, V3_VERB, "SUSPEND (%.2fs since last cb)\n", elapsed);

            _suspendCond.wait(_suspendMutex, [this] { return !_suspend; });
            LOGGER(_logger, V4_VVER, "RESUME\n");

            if (_stop) {
                LOGGER(_logger, V4_VVER, "STOP after suspension\n", elapsed);
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
    Logger &_logger;
    double _lastTermCallbackTime;

    int _stop = 0;
    volatile bool _suspend = false;

    Mutex _suspendMutex;
    ConditionVariable _suspendCond;
};
