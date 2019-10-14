
#ifndef DOMPASCH_BALANCER_EPOCH_COUNTER_H
#define DOMPASCH_BALANCER_EPOCH_COUNTER_H

class EpochCounter {

private:
    int epoch;

public:
    EpochCounter() : epoch(0) {};
    int get() const {return epoch;}
    void increment() {epoch++;}
};

#endif