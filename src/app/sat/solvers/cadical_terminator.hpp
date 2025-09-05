
#pragma once

#include "app/sat/solvers/solving_replay.hpp"
#include "util/logger.hpp"
#include "util/sys/timer.hpp"

#include "cadical/src/cadical.hpp"

struct CadicalTerminator : public CaDiCaL::Terminator {
    CadicalTerminator(Logger &logger, SolvingReplay& replay) : _logger(logger), _replay(replay) {
        _lastTermCallbackTime = Timer::elapsedSeconds();
    };
    ~CadicalTerminator() override {}

    bool terminate() override {
        if (_replay.getMode() == SolvingReplay::REPLAY)
            return _replay.replayTerminateCallback();

        double time = Timer::elapsedSeconds();
        double elapsed = time - _lastTermCallbackTime;
        _lastTermCallbackTime = time;

        bool terminate = _stop || (_ext_terminator && _ext_terminator());
        if (_replay.getMode() == SolvingReplay::RECORD)
            _replay.recordTerminateCallback(terminate);
        if (terminate) {
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
    SolvingReplay& _replay;
    double _lastTermCallbackTime;
    int _stop = 0;
    std::function<bool(void)> _ext_terminator;
};
