
#ifndef DOMPASCH_MALLOB_TIME_PERIOD_HPP
#define DOMPASCH_MALLOB_TIME_PERIOD_HPP

#include <string>

#include "util/ctre.hpp"

static constexpr ctll::fixed_string REGEX_TIME_PERIOD = ctll::fixed_string{ "([0-9\\.]+)(ms|s|m|h|d)?" };

class TimePeriod {

public:
    enum Unit {MILLISECONDS, SECONDS, MINUTES, HOURS, DAYS};

private:
    float _value_seconds;

public:
    TimePeriod(const std::string& description) {
        auto match = ctre::match<REGEX_TIME_PERIOD>(description);
        if (!match) abort();

        float val = std::stof(match.get<1>().to_string());
        std::string unit = match.get<2>().to_string();

        if (unit == "ms") _value_seconds = val / 1000;
        else if (unit == "s") _value_seconds = val;
        else if (unit == "m") _value_seconds = val * 60;
        else if (unit == "h") _value_seconds = val * 60 * 60;
        else if (unit == "d") _value_seconds = val * 60 * 60 * 24;
        else {
            unit = "s";
            _value_seconds = val;
        }
    }

    float get(Unit unit) {
        if (unit == MILLISECONDS) return _value_seconds * 1000;
        else if (unit == SECONDS) return _value_seconds;
        else if (unit == MINUTES) return _value_seconds / 60;
        else if (unit == HOURS) return _value_seconds / (60 * 60);
        else if (unit == DAYS) return _value_seconds / (60 * 60 * 24);
    }
};

#endif
