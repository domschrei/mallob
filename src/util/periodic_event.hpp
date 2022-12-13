
#ifndef DOMPASCH_MALLOB_PERIODIC_EVENT_HPP
#define DOMPASCH_MALLOB_PERIODIC_EVENT_HPP

#include "util/sys/timer.hpp"

template <int PeriodMillis>
class PeriodicEvent {

private:
    float _last_event_time;

public:
    PeriodicEvent() {
        _last_event_time = Timer::elapsedSecondsCached();
    }

    bool ready(float time = -1) {
        if (time < 0) time = Timer::elapsedSecondsCached();
        if (time - _last_event_time >= 0.001f*PeriodMillis) {
            _last_event_time = time;
            return true;
        }
        return false;
    }
};

#endif
