
#pragma once

#include "util/logger.hpp"
#include "util/sys/timer.hpp"

#include "cadical/src/cadical.hpp"

struct CadicalTerminator : public CaDiCaL::Terminator {
    CadicalTerminator(Logger &logger) : _logger(logger) {
        _lastTermCallbackTime = Timer::elapsedSeconds();
    };
    ~CadicalTerminator() override {}

    bool terminate() override {

        double time = Timer::elapsedSeconds();
        double elapsed = time - _lastTermCallbackTime;
        _lastTermCallbackTime = time;

        if (_stop || (_ext_terminator && _ext_terminator())) {
            LOGGER(_logger, V4_VVER, "STOP (%.2fs since last cb)\n", elapsed);
            return true;
        }
        return false;
    }

    void setInterrupt() {
        _stop = 1;
    }
    void unsetInterrupt() {
        _stop = 0;
    }

    void setExternalTerminator(const std::function<bool(void)>& ext) {
        _ext_terminator = ext;
    }

private:
    Logger &_logger;
    double _lastTermCallbackTime;
    int _stop = 0;
    std::function<bool(void)> _ext_terminator;
};
