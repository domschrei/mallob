
#ifndef DOMPASCH_TIMER_H
#define DOMPASCH_TIMER_H

#include <sys/time.h>
#include <ctime>

class Parameters; // forward declaration

class Timer {

private:
    static timespec timespecStart, timespecEnd;
    static float lastTimeMeasured;

public:
    static void init();
    static void init(timespec start);

    /**
     * Returns elapsed time since program start (since MyMpi::init) in seconds.
     */
    static inline float elapsedSeconds() {
        clock_gettime(CLOCK_MONOTONIC_RAW, &timespecEnd);
        float time = timespecEnd.tv_sec - timespecStart.tv_sec  
            + (0.001f * 0.001f * 0.001f) * (timespecEnd.tv_nsec - timespecStart.tv_nsec);
        return time;
    }

    /**
     * Cache the current value of elapsedSeconds() to later be queried without
     * any cost via "elapsedSecondsCached()". Call this method as well as
     * "elapsedSecondsCached()" from the main thread ONLY.
     */
    static inline void cacheElapsedSeconds() {
        lastTimeMeasured = elapsedSeconds();
    }

    /**
     * Returns elapsed time since program start (since MyMpi::init) in seconds
     * at the last time "cacheElapsedSeconds()" was called.
     * NOT thread-safe; only call from the main thread.
     */
    static inline float elapsedSecondsCached() {
        return lastTimeMeasured;
    }

    static timespec getStartTime();
};

#endif