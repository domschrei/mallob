
#include <sys/time.h>

#include "timer.hpp"
#include "util/params.hpp"

timespec Timer::timespecStart;
timespec Timer::timespecEnd;

void Timer::init() {
    clock_gettime(CLOCK_MONOTONIC_RAW, &timespecStart);
}
void Timer::init(timespec start) {
    timespecStart = start;
}

bool Timer::globalTimelimReached(Parameters& params) {
    return params.timeLimit() > 0 && elapsedSeconds() > params.timeLimit();
}

timespec Timer::getStartTime() {
    return timespecStart;
}