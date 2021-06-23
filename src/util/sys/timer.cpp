
#include <chrono>

#include "timer.hpp"

using namespace std::chrono;
double startTime;

double Timer::now() {
    return 0.001 * 0.001 * 0.001 * system_clock::now().time_since_epoch().count();
}

void Timer::init(double start) {
    startTime = start == -1 ? now() : now()-start;
}

/**
 * Returns elapsed time since program start (since MyMpi::init) in seconds.
 */
float Timer::elapsedSeconds() {
    return now() - startTime;
}

bool Timer::globalTimelimReached(Parameters& params) {
    return params.timeLimit() > 0 && elapsedSeconds() > params.timeLimit();
}

double Timer::getStartTime() {
    return startTime;
}