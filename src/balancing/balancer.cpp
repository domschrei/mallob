
#include "balancer.h"

int Balancer::getVolume(int jobId) {
    if (!_volumes.count(jobId))
        _volumes[jobId] = 1;
    return _volumes[jobId];
}

void Balancer::updateVolume(int jobId, int volume) {
    _volumes[jobId] = volume;
}

float Balancer::allReduce(float contribution) const {
    float result;
    MPI_Allreduce(&contribution, &result, 1, MPI_FLOAT, MPI_SUM, _comm);
    _stats.increment("reductions");
    _stats.increment("broadcasts");
    return result;
}
void Balancer::iAllReduce(float contribution) {
    _reduce_contrib = contribution;
    _reduce_result = contribution;
    _reduce_request = MPI_Request();
    MPI_Iallreduce(&_reduce_contrib, &_reduce_result, 1, MPI_FLOAT, MPI_SUM, _comm, &_reduce_request);
    _stats.increment("reductions");
    _stats.increment("broadcasts");
}
float Balancer::reduce(float contribution, int rootRank) const {
    float result;
    MPI_Reduce(&contribution, &result, 1, MPI_FLOAT, MPI_SUM, rootRank, _comm);
    _stats.increment("reductions");
    return result;
}
void Balancer::iReduce(float contribution, int rootRank) {
    _reduce_result = 0;
    _reduce_request = MPI_Request();
    MPI_Ireduce(&contribution, &_reduce_result, 1, MPI_FLOAT, MPI_SUM, rootRank, _comm, &_reduce_request);
    _stats.increment("reductions");
}

int Balancer::getDemand(const Job& job) {
    return job.getDemand(getVolume(job.getId()));
}