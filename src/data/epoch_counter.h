
#ifndef DOMPASCH_BALANCER_EPOCH_COUNTER_H
#define DOMPASCH_BALANCER_EPOCH_COUNTER_H

#include "util/timer.h"

class EpochCounter {

private:
    int epoch;
    float lastSync;

public:
    EpochCounter() : epoch(0), lastSync(0) {};

    int getEpoch() const {return epoch;}
    int getSecondsSinceLastSync() const {return Timer::elapsedSeconds() - lastSync;};

    void increment() {epoch++;}
    void resetLastSync() {lastSync = Timer::elapsedSeconds();}; 
};

#endif