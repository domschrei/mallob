
#include "balancer.h"

int Balancer::getVolume(int jobId) {
    if (!_volumes.count(jobId))
        return 1;
    return _volumes[jobId];
}

bool Balancer::hasVolume(int jobId) {
    return _volumes.count(jobId);
}

void Balancer::updateVolume(int jobId, int volume) {
    _volumes[jobId] = volume;
}

void Balancer::iReduce(float contribution, int rootRank) {
    _reduce_contrib = contribution;
    _reduce_result = 0;
    _reduce_request = MyMpi::ireduce(_comm, &_reduce_contrib, &_reduce_result, rootRank);
    _stats.increment("reductions");
}
void Balancer::iAllReduce(float contribution) {
    _reduce_contrib = contribution;
    _reduce_result = contribution;
    _reduce_request = MyMpi::iallreduce(_comm, &_reduce_contrib, &_reduce_result);
    _stats.increment("reductions");
    _stats.increment("broadcasts");
}

int Balancer::getDemand(const Job& job) {
    return job.getDemand(getVolume(job.getId()));
}