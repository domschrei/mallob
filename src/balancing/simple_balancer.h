
#ifndef DOMPASCH_BALANCER_SIMPLE_H
#define DOMPASCH_BALANCER_SIMPLE_H

#include "balancing/balancer.h"

class SimpleBalancer : public Balancer {

public:
    SimpleBalancer(MPI_Comm& comm, Parameters params) : Balancer(comm, params) {
        
    }
    std::map<int, int> balance(std::map<int, JobImage*>& jobs) override;
};

#endif