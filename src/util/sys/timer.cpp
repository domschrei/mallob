
#include <sys/time.h>

#include "timer.hpp"
#include "util/params.hpp"

timespec Timer::timespecStart;
timespec Timer::timespecEnd;
float Timer::lastTimeMeasured {0};

void Timer::init() {
    clock_gettime(CLOCK_MONOTONIC_RAW, &timespecStart);
    cacheElapsedSeconds();
}
void Timer::init(timespec start) {
    timespecStart = start;
    cacheElapsedSeconds();
}

timespec Timer::getStartTime() {
    return timespecStart;
}