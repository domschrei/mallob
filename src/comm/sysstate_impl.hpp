
#ifndef DOMPASCH_MALLOB_SYSSTATE_IMPL_HPP
#define DOMPASCH_MALLOB_SYSSTATE_IMPL_HPP

#include "sysstate.hpp"

template <int N>
SysState<N>::SysState(MPI_Comm& comm, float period, SysStateCollective collective, MPI_Op operation): 
        _comm(comm), _period(period), _collective(collective), _op(operation) {
    
    _global_state.resize(N * (_collective == ALLGATHER ? MyMpi::size(comm) : 1), 0);
    for (size_t i = 0; i < N; i++)
        _local_state[i] = 0.0f;
}

template <int N>
bool SysState<N>::isAggregating() const {return _aggregating;}

template <int N>
bool SysState<N>::canStartAggregating(float time) const {
    return !isAggregating() && time-_last_aggregation >= _period;
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

    float time = elapsedTime < 0 ? Timer::elapsedSecondsCached() : elapsedTime;
    if (time-_last_check > _period/5) {
        _last_check = time;
        float timeSinceLast = time-_last_aggregation;
        if (!_aggregating && timeSinceLast >= _period) {
            _last_aggregation = time;
            if (_collective == ALLREDUCE) {
                _request = MyMpi::iallreduce(_comm, _local_state, _global_state.data(), N, _op);
            } else /*allgather*/ {
                _request = MyMpi::iallgather(_comm, _local_state, _global_state.data(), N);
            }
            _aggregating = true;
        } else if (_aggregating) {
            MPI_Status status;
            int flag;
            MPI_Test(&_request, &flag, &status);
            if (flag) {
                _aggregating = false;
                return true;
            } else if (SysState_isUnresponsiveNodeCrashingEnabled()
                    && _last_aggregation > 0 && timeSinceLast > 60) {
                LOG(V0_CRIT, "[ERROR] Unresponsive node(s)\n");
                Logger::getMainInstance().flush();
                abort();
            }
        }
    }
    return false;
}

template <int N>
const std::vector<float>& SysState<N>::getGlobal() {
    return _global_state;
}

template <int N>
float* SysState<N>::getLocal() {
    return _local_state;
}

#endif