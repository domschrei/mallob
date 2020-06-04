
#ifndef DOMPASCH_BALANCER_EPOCH_COUNTER_H
#define DOMPASCH_BALANCER_EPOCH_COUNTER_H

#include "util/sys/timer.hpp"

class EpochCounter {

private:
    int _epoch;
    float _last_sync;
    float _last_epoch_duration;

public:
    EpochCounter() : _epoch(0), _last_sync(0), _last_epoch_duration(0) {}

    int getEpoch() const {return _epoch;}
    float getSecondsSinceLastSync() const {return Timer::elapsedSeconds() - _last_sync;}
    float getLastEpochDuration() const {return _last_epoch_duration;}

    void increment() {_epoch++;}
    void resetLastSync() {
        _last_epoch_duration = Timer::elapsedSeconds()-_last_sync; 
        _last_sync = Timer::elapsedSeconds();
    }
};

#endif