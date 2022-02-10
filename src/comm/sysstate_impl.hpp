
#ifndef DOMPASCH_MALLOB_SYSSTATE_IMPL_HPP
#define DOMPASCH_MALLOB_SYSSTATE_IMPL_HPP

#include "sysstate.hpp"

template <int N>
SysState<N>::SysState(MPI_Comm& comm, float period): _comm(comm), _period(period) {
    for (int i = 0; i < N; i++) {
        _local_state[i] = 0.0f;
        _global_state[i] = 0.0f;
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

    float time = elapsedTime < 0 ? Timer::elapsedSeconds() : elapsedTime;
    if (time-_last_check > _period/5) {
        _last_check = time;
        float timeSinceLast = time-_last_aggregation;
        if (!_aggregating && timeSinceLast >= _period) {
            _last_aggregation = time;
            _request = MyMpi::iallreduce(_comm, _local_state, _global_state, N);
            _aggregating = true;
        } else if (_aggregating) {
            MPI_Status status;
            int flag;
            MPI_Test(&_request, &flag, &status);
            if (flag) {
                _aggregating = false;
                return true;
            } else if (_last_aggregation > 0 && timeSinceLast > 60) {
                LOG(V0_CRIT, "[ERROR] Unresponsive node(s)\n");
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