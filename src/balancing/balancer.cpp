
#include "balancer.hpp"

void Balancer::forget(int jobId) {
    _jobs_being_balanced.erase(jobId);
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
    return job.getDemand(job.getVolume());
}