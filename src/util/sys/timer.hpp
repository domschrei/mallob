
#ifndef DOMPASCH_TIMER_H
#define DOMPASCH_TIMER_H

#include "util/params.hpp"

class Timer {

public:
    static void init();

    /**
     * Returns elapsed time since program start (since MyMpi::init) in seconds.
     */
    static float elapsedSeconds();

    static bool globalTimelimReached(Parameters& params);
};

#endif