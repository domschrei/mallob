
#ifndef DOMPASCH_BALANCER_INTERFACE_H
#define DOMPASCH_BALANCER_INTERFACE_H

#include <map>

#include "data/job.h"
#include "data/statistics.h"
#include "util/mpi.h"
#include "util/params.h"

class Balancer {

public:
    Balancer(MPI_Comm& comm, Parameters& params, Statistics& stats) : 
    comm(comm), params(params), stats(stats), loadFactor(params.getFloatParam("l")) {}
    
    virtual std::map<int, int> balance(std::map<int, Job*>& jobs) = 0;
    int getVolume(int jobId);
    void updateVolume(int jobId, int volume);

protected:
    float allReduce(float contribution) const;
    float reduce(float contribution, int rootRank) const;
    int getDemand(const Job& job);

protected:
    MPI_Comm& comm;
    Parameters& params;
    Statistics& stats;
    float loadFactor;

    std::map<int, int> volumes;
};

#endif