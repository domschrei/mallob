
#ifndef DOMPASCH_MALLOB_SYSSTATE_HPP
#define DOMPASCH_MALLOB_SYSSTATE_HPP

#include "comm/mympi.hpp"

template <int N>
class SysState {

private:
    MPI_Comm& _comm;
    float _period;
    
    float _local_state[N];
    float _global_state[N];
    MPI_Request _request;
    bool _aggregating = false;

    float _last_aggregation = 0;
    float _last_check = 0;

public:
    SysState(MPI_Comm& comm, float period);
    void setLocal(int pos, float val);
    void setLocal(std::initializer_list<float> elems);
    void addLocal(int pos, float val);
    bool aggregate(float elapsedTime = -1);
    float* getGlobal();
};

#include "sysstate_impl.hpp"

#endif