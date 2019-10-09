
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
    return result;
}
float Balancer::reduce(float contribution, int rootRank) const {
    float result;
    MPI_Reduce(&contribution, &result, 1, MPI_FLOAT, MPI_SUM, rootRank, comm);
    return result;
}

int Balancer::getDemand(const JobImage& job) {
    // Twice as much (+1) than previous volume, 
    // at most the amount of workers
    return std::min(2 * getVolume(job.getJob().getId()) + 1, MyMpi::size(comm));
}