
#ifndef DOMPASCH_MALLOB_PERIODIC_EVENT_HPP
#define DOMPASCH_MALLOB_PERIODIC_EVENT_HPP

#include "util/sys/timer.hpp"
#include <algorithm>

template <int PeriodMillis, int InitPeriodMillis = -1>
class PeriodicEvent {

private:
    float _last_event_time;
    float _period;

public:
    PeriodicEvent() {
        if constexpr (InitPeriodMillis == -1) {
            _period = 0.001f * PeriodMillis;
        } else {
            static_assert(InitPeriodMillis < PeriodMillis);
            _period = 0.001f * InitPeriodMillis;
        }
        _last_event_time = Timer::elapsedSecondsCached();
    }

    bool ready(float time = -1) {
        if (time < 0) time = Timer::elapsedSecondsCached();
        if (time - _last_event_time >= _period) {
            _last_event_time = time;

            if constexpr (InitPeriodMillis != -1) {
                float limit = 0.001f * PeriodMillis;
                if (_period < limit) {
                    _period = std::min(1.2f * _period, limit);
                }
            }

            return true;
        }
        return false;
    }

    void resetToInitPeriod() {
        static_assert(InitPeriodMillis != -1);
        _period = 0.001f * InitPeriodMillis;
    }
};

#endif
