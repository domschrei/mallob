
#ifndef DOMPASCH_TIMER_H
#define DOMPASCH_TIMER_H

class Parameters; // forward declaration

class Timer {

public:
    static void init(double start = -1);

    /**
     * Returns elapsed time since program start (since MyMpi::init) in seconds.
     */
    static float elapsedSeconds();

    static bool globalTimelimReached(Parameters& params);

    static double getStartTime();

private:
    static double now();

};

#endif