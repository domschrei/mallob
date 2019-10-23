
#include <chrono>

#include "timer.h"

using namespace std::chrono;
high_resolution_clock::time_point startTime;

void Timer::init() {
    startTime = high_resolution_clock::now();
}

/**
 * Returns elapsed time since program start (since MyMpi::init) in seconds.
 */
float Timer::elapsedSeconds() {
    high_resolution_clock::time_point nowTime = high_resolution_clock::now();
    duration<double, std::milli> time_span = nowTime - startTime;
    return time_span.count() / 1000;
}