
#ifndef DOMPASCH_MALLOB_SYSSTATE_HPP
#define DOMPASCH_MALLOB_SYSSTATE_HPP

#include "comm/mympi.hpp"

#include <vector>

template <int N>
class SysState {

public:
    enum SysStateCollective {ALLGATHER, ALLREDUCE};

private:
    MPI_Comm& _comm;
    float _period;
    SysStateCollective _collective;
    MPI_Op _op;

    float _local_state[N];
    std::vector<float> _global_state;
    MPI_Request _request = MPI_REQUEST_NULL;
    bool _aggregating = false;

    float _last_aggregation = 0;
    float _last_check = 0;

public:
    SysState(MPI_Comm& comm, float period, SysStateCollective collective, MPI_Op operation = MPI_SUM);
    bool isAggregating() const;
    bool canStartAggregating(float time) const;
    void setLocal(int pos, float val);
    void setLocal(std::initializer_list<float> elems);
    void addLocal(int pos, float val);
    bool aggregate(float elapsedTime = -1);
    float* getLocal();
    const std::vector<float>& getGlobal();
};

#include "sysstate_impl.hpp"

#endif