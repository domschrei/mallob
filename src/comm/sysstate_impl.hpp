
#ifndef DOMPASCH_MALLOB_SYSSTATE_IMPL_HPP
#define DOMPASCH_MALLOB_SYSSTATE_IMPL_HPP

#include "sysstate.hpp"

template <int N>
SysState<N>::SysState(MPI_Comm& comm): _comm(comm) {
    for (int i = 0; i < N; i++) {
        _local_state[i] = 0.0f;
    }
}

template <int N>
void SysState<N>::setLocal(std::initializer_list<float> elems) {
    int i = 0;
    for (float& elem : elems) {
        _local_state[i++] = elem;
    }
}

template <int N>
void SysState<N>::setLocal(int pos, float val) {
    _local_state[pos] = val;
}

template <int N>
void SysState<N>::addLocal(int pos, float val) {
    _local_state[pos] += val;
}

template <int N>
bool SysState<N>::aggregate(float elapsedTime) {

    float time = elapsedTime;
    if (time-_last_check > 0.1) {
        _last_check = time;
        float timeSinceLast = time-_last_aggregation;
        if (!_aggregating && timeSinceLast >= 1.0) {
            _last_aggregation = time;
            _request = MyMpi::iallreduce(_comm, _local_state, _global_state, N);
            _aggregating = true;
        } else if (_aggregating) {
            MPI_Status status;
            bool done = MyMpi::test(_request, status);
            if (done) {
                _aggregating = false;
                return true;
            } else if (_last_aggregation > 0 && timeSinceLast > 300) {
                log(V0_CRIT, "ERROR: Unresponsive node(s) since 300 seconds! Aborting\n");
                Logger::getMainInstance().flush();
                abort();
            }
        }
    }
    return false;
}

template <int N>
float* SysState<N>::getGlobal() {
    return _global_state;
}

#endif