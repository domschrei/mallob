
#ifndef DOMPASCH_TIMER_H
#define DOMPASCH_TIMER_H

#include <sys/time.h>
#include <ctime>

class Parameters; // forward declaration

class Timer {

private:
    static timespec timespecStart, timespecEnd;

public:
    static void init();
    static void init(timespec start);

    /**
     * Returns elapsed time since program start (since MyMpi::init) in seconds.
     */
    static inline float elapsedSeconds() {
        clock_gettime(CLOCK_MONOTONIC_RAW, &timespecEnd);
        return timespecEnd.tv_sec - timespecStart.tv_sec  
            + (0.001f * 0.001f * 0.001f) * (timespecEnd.tv_nsec - timespecStart.tv_nsec);
    }

    static bool globalTimelimReached(Parameters& params);

    static timespec getStartTime();
};

#endif