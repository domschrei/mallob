
#ifndef DOMPASCH_BALANCER_INTERFACE_H
#define DOMPASCH_BALANCER_INTERFACE_H

#include <map>

#include "data/job_image.h"
#include "util/mpi.h"
#include "util/params.h"

class Balancer {

public:
    Balancer(MPI_Comm& comm, Parameters params) : 
    comm(comm), params(params), loadFactor(params.getFloatParam("l")) {}
    
    virtual std::map<int, int> balance(std::map<int, JobImage*>& jobs) = 0;

    int getVolume(int jobId) {
        if (!volumes.count(jobId))
            volumes[jobId] = 1;
        return volumes[jobId];
    }

protected:
    float allReduce(float contribution) const {
        float result;
        MPI_Allreduce(&contribution, &result, 1, MPI_FLOAT, MPI_SUM, comm);
        return result;
    }
    float reduce(float contribution, int rootRank) const {
        float result;
        MPI_Reduce(&contribution, &result, 1, MPI_FLOAT, MPI_SUM, rootRank, comm);
        return result;
    }

    int getDemand(Job& job) {
        // Twice as much (+1) than previous volume, 
        // at most the amount of workers
        return std::min(2 * getVolume(job.getId()) + 1, MyMpi::size(comm));
    }

protected:
    MPI_Comm& comm;
    Parameters& params;
    float loadFactor;

    std::map<int, int> volumes;
};

#endif