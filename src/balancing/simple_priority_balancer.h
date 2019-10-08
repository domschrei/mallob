
#ifndef DOMPASCH_BALANCER_SIMPLE_PRIORITY_H
#define DOMPASCH_BALANCER_SIMPLE_PRIORITY_H

#include "balancing/balancer.h"

class SimplePriorityBalancer : public Balancer {

public:
    SimplePriorityBalancer(MPI_Comm& comm, Parameters params) : Balancer(comm, params) {
        
    }
    std::map<int, int> balance(std::map<int, JobImage*>& jobs) override;
};

#endif