
#pragma once

#include "data/reduceable.hpp"
#include "mympi.hpp"
#include "util/tsl/robin_map.h"

extern unsigned long _all_reduction_running_id;

// T must be a subclass of Reduceable (see data/reduceable.hpp).
// An arbitrary number of all-reductions can be performed with a single instance of this class.
// These all-reductions can also be performed concurrently.
// In addition, each MPI process can hold multiple instances of this class.
// This flexibility has the trade-off that constructing an instance of this class requires
// an *instance ID* which must be the same across all MPI processes in order to participate
// in the same all-reduction. In addition, each allReduce call must be provided a *call ID*
// which likewise must be the same across all MPI processes participating in the all-reduction.
template <class T>
class AllReduction {

private:
    MPI_Comm _comm;
    MessageQueue& _msg_q;
    // References for the message callbacks which need to be deleted at destruction
    MessageQueue::CallbackRef _ref_up;
    MessageQueue::CallbackRef _ref_down;
    // ID which all corresponding AllReduction instances across the MPI processes share
    int _instance_id;

    int _my_rank;
    int _comm_size;
    std::vector<int> _world_ranks;

    int _num_desired_contribs;

    // An ID is assigned to each all-reduction call.
    struct AllReduceState {
        int id;
        std::function<void(T&)> cbResult;
        T contrib;
        int numArrivedContribs {0};
    };
    // Maintain a "state" struct for each active all-reduction ID.
    tsl::robin_map<int, AllReduceState> _states_by_id;

public:
    AllReduction<T>(MPI_Comm comm, MessageQueue& msgQ, int instanceId) : 
            _comm(comm), _msg_q(msgQ), _instance_id(instanceId) {

        _my_rank = MyMpi::rank(_comm);
        _comm_size = MyMpi::size(_comm);

        // How many contributions should this process expect?
        _num_desired_contribs = 1; // yourself
        if (getLeftChildRank(_my_rank) < _comm_size) _num_desired_contribs++;
        if (getRightChildRank(_my_rank) < _comm_size) _num_desired_contribs++;

        _ref_up = msgQ.registerCallback(MSG_ALL_REDUCTION_UP, [&](auto& h) {handle(h);});
        _ref_down = msgQ.registerCallback(MSG_ALL_REDUCTION_DOWN, [&](auto& h) {handle(h);});

        // Create a mapping from the communicator's ranks to world ranks
        // (since MessageQueue works with global ranks exclusively)
        MPI_Group groupComm;
        MPI_Group groupWorld;
        MPI_Comm_group(_comm, &groupComm);
        MPI_Comm_group(MPI_COMM_WORLD, &groupWorld);
        std::vector<int> localRanks(_comm_size);
        for (size_t i = 0; i < _comm_size; i++) localRanks[i] = i;
        _world_ranks.resize(_comm_size);
        MPI_Group_translate_ranks(groupComm, localRanks.size(), localRanks.data(), 
            groupWorld, _world_ranks.data());
    }

    ~AllReduction() {
        _msg_q.clearCallback(MSG_ALL_REDUCTION_UP, _ref_up);
        _msg_q.clearCallback(MSG_ALL_REDUCTION_DOWN, _ref_down);
    }

    // Begin a new all-reduction with the provided local contribution
    // and a callback which will be called when the all-reduction is finished.
    // The callback will be called from the main thread.
    void allReduce(int callId, const T& contribution, std::function<void(T&)> callbackOnResult) {

        auto& state = _states_by_id[callId];
        state.id = callId;
        state.cbResult = callbackOnResult;
        state.contrib.aggregate(contribution);
        state.numArrivedContribs++;

        if (state.numArrivedContribs == _num_desired_contribs) forward(state);
    }

    // MessageHandles of tags MSG_ALL_REDUCTION_{UP,DOWN} should be routed to here.
    void handle(MessageHandle& h) {

        if (h.tag != MSG_ALL_REDUCTION_UP && h.tag != MSG_ALL_REDUCTION_DOWN) return;

        // Extract all-reduction instance ID and call ID
        auto data = h.moveRecvData();
        assert(data.size() > 2*sizeof(int));
        int instanceId, callId;
        memcpy(&instanceId, data.data()+data.size()-2*sizeof(int), sizeof(int));
        memcpy(&callId, data.data()+data.size()-sizeof(int), sizeof(int));

        if (instanceId != _instance_id) return;
        
        // As the instance ID is correct, we can assume that we can deserialize 
        // the received object as an instance of class T
        auto& state = _states_by_id[callId];
        state.id = callId;
        std::vector<uint8_t> dataWithoutId(data.data(), data.data()+data.size()-2*sizeof(int));
        auto elem = Serializable::get<T>(dataWithoutId);

        // Reduction
        if (h.tag == MSG_ALL_REDUCTION_UP) {
            state.contrib.aggregate(elem);
            state.numArrivedContribs++;
            if (state.numArrivedContribs == _num_desired_contribs) forward(state);
        }
        
        // Broadcast
        if (h.tag == MSG_ALL_REDUCTION_DOWN) {
            // forward to children
            if (getLeftChildRank(_my_rank) < _comm_size)
                MyMpi::isendCopy(_world_ranks[getLeftChildRank(_my_rank)], MSG_ALL_REDUCTION_DOWN, data);
            if (getRightChildRank(_my_rank) < _comm_size)
                MyMpi::isendCopy(_world_ranks[getRightChildRank(_my_rank)], MSG_ALL_REDUCTION_DOWN, data);
            state.cbResult(elem); // publish result
            _states_by_id.erase(callId); // clean up
        }
    }

private:
    void forward(AllReduceState& state) {

        // Prepare serialized data, followed by reduction ID
        auto packed = state.contrib.serialize();
        auto sizeBefore = packed.size();
        packed.resize(sizeBefore + 2*sizeof(int));
        memcpy(packed.data() + sizeBefore, &_instance_id, sizeof(int));
        memcpy(packed.data() + sizeBefore + sizeof(int), &state.id, sizeof(int));

        if (_my_rank == 0) {
            // Root: switch to broadcast via a self message
            MyMpi::isend(_world_ranks[_my_rank], MSG_ALL_REDUCTION_DOWN, std::move(packed));
        } else {
            // aggregate upwards
            MyMpi::isend(_world_ranks[getParentRank(_my_rank)], MSG_ALL_REDUCTION_UP, std::move(packed));
        }
        state.numArrivedContribs = 0;
    }

    static int getLeftChildRank(int rank) {return 2*rank+1;}
    static int getRightChildRank(int rank) {return 2*rank+2;}
    static int getParentRank(int rank) {return (rank-1)/2;}

};
