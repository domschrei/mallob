
#include "balancer.h"

int Balancer::getVolume(int jobId) {
    if (!volumes.count(jobId))
        volumes[jobId] = 1;
    return volumes[jobId];
}

void Balancer::updateVolume(int jobId, int volume) {
    volumes[jobId] = volume;
}

float Balancer::allReduce(float contribution) const {
    float result;
    MPI_Allreduce(&contribution, &result, 1, MPI_FLOAT, MPI_SUM, comm);
    stats.increment("reductions");
    stats.increment("broadcasts");
    return result;
}
float Balancer::reduce(float contribution, int rootRank) const {
    float result;
    MPI_Reduce(&contribution, &result, 1, MPI_FLOAT, MPI_SUM, rootRank, comm);
    stats.increment("reductions");
    return result;
}

int Balancer::getDemand(const Job& job) {
    // Twice as much (+1) than previous volume, 
    // at most the amount of workers
    return job.getDemand();
}