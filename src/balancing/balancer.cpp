
#include "balancer.hpp"

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

void Balancer::forget(int jobId) {
    _jobs_being_balanced.erase(jobId);
    _volumes.erase(jobId);
    _priorities.erase(jobId);
    _demands.erase(jobId);
    _temperatures.erase(jobId);
}

void Balancer::iReduce(float contribution, int rootRank) {
    _reduce_contrib = contribution;
    _reduce_result = 0;
    _reduce_request = MyMpi::ireduce(_comm, &_reduce_contrib, &_reduce_result, rootRank);
}
void Balancer::iAllReduce(float contribution) {
    _reduce_contrib = contribution;
    _reduce_result = contribution;
    _reduce_request = MyMpi::iallreduce(_comm, &_reduce_contrib, &_reduce_result);
}

int Balancer::getDemand(const Job& job) {
    return job.getDemand(getVolume(job.getId()));
}