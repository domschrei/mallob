
#ifndef DOMPASCH_BALANCER_EPOCH_COUNTER_H
#define DOMPASCH_BALANCER_EPOCH_COUNTER_H

#include "util/timer.h"

class EpochCounter {

private:
    int _epoch;
    float _last_sync;

public:
    EpochCounter() : _epoch(0), _last_sync(0) {}

    int getEpoch() const {return _epoch;}
    int getSecondsSinceLastSync() const {return Timer::elapsedSeconds() - _last_sync;}

    void increment() {_epoch++;}
    void resetLastSync() {_last_sync = Timer::elapsedSeconds();}
};

#endif