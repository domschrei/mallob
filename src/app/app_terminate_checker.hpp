
#pragma once

#include "data/job_description.hpp"
#include "util/params.hpp"
#include "util/sys/terminator.hpp"
#include "util/sys/timer.hpp"

struct AppTerminateChecker {

private:
    const Parameters& _params;
    const JobDescription& _desc;
    const float _time_of_termination {-1};

public:
    AppTerminateChecker(const Parameters& params, const JobDescription& desc) :
        _params(params), _desc(desc), _time_of_termination(computeEndTime()) {}

    inline bool isTimeoutHit(float endTime = -1) const {
        if (Terminator::isTerminating()) {
            return true;
        }
        if (endTime < 0) endTime = _time_of_termination;
        if (Timer::elapsedSeconds() > endTime) {
            return true;
        }
        return false;
    }

    float getEndTime() const {
        return _time_of_termination;
    }

private:
    float computeEndTime() {
        float startTime = Timer::elapsedSeconds();
        float endTime = INT32_MAX;
        if (_params.timeLimit() > 0)
            endTime = std::min(endTime, startTime + _params.timeLimit());
        if (_desc.getWallclockLimit() > 0)
            endTime = std::min(endTime, startTime + _desc.getWallclockLimit());
        return endTime;
    }
};
