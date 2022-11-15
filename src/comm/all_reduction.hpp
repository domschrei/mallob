
#pragma once

#include "data/reduceable.hpp"
#include "mympi.hpp"
#include "util/tsl/robin_map.h"

// T: must be subclass of Reduceable (see data/reduceable.hpp).
// An arbitrary number of all-reductions can be performed with a single instance of this class.
// These all-reductions can also be performed concurrently.
// Before ANY process calls allReduce(), all processes must have registered callbacks 
// for tags MSG_ALL_REDUCTION_{UP,DOWN} to be forwarded to the handle() method of their 
// all-reduction instance.
template <class T>
class AllReduction {

private:
    MPI_Comm _comm;
    int _my_rank;
    int _comm_size;
    int _num_desired_contribs;

    // An ID is assigned to each all-reduction.
    unsigned long _running_id {1};
    struct AllReduceState {
        unsigned long id;
        std::function<void(T&)> cbResult;
        T contrib;
        int numArrivedContribs {0};
    };
    // Maintain a "state" struct for each active all-reduction ID.
    tsl::robin_map<unsigned long, AllReduceState> _states_by_id;

public:
    AllReduction<T>(MPI_Comm comm) : _comm(comm) {

        _my_rank = MyMpi::rank(_comm);
        _comm_size = MyMpi::size(_comm);

        // How many contributions should this process expect?
        _num_desired_contribs = 1; // yourself
        if (getLeftChildRank(_my_rank) < _comm_size) _num_desired_contribs++;
        if (getRightChildRank(_my_rank) < _comm_size) _num_desired_contribs++;
    }

    // Begin a new all-reduction with the provided local contribution
    // and a callback which will be called when the all-reduction is finished.
    // The callback will be called from the main thread.
    void allReduce(const T& contribution, std::function<void(T&)> callbackOnResult) {

        auto thisId = _running_id++;

        auto& state = _states_by_id[thisId];
        state.id = thisId;
        state.cbResult = callbackOnResult;
        state.contrib.merge(contribution);
        state.numArrivedContribs++;

        if (state.numArrivedContribs == _num_desired_contribs) forward(state);
    }

    // MessageHandles of tags MSG_ALL_REDUCTION_{UP,DOWN} should be routed to here.
    void handle(MessageHandle& h) {

        if (h.tag != MSG_ALL_REDUCTION_UP && h.tag != MSG_ALL_REDUCTION_DOWN) return;

        // Extract all-reduction ID
        auto data = h.moveRecvData();
        assert(data.size() > sizeof(unsigned long));
        unsigned long id;
        memcpy(&id, data.data()+data.size()-sizeof(unsigned long), sizeof(unsigned long));
        
        auto& state = _states_by_id[id];
        state.id = id;
        std::vector<uint8_t> dataWithoutId(data.data(), data.data()+data.size()-sizeof(unsigned long));
        auto elem = Serializable::get<T>(dataWithoutId);

        // Reduction
        if (h.tag == MSG_ALL_REDUCTION_UP) {
            state.contrib.merge(elem);
            state.numArrivedContribs++;
            if (state.numArrivedContribs == _num_desired_contribs) forward(state);
        }
        
        // Broadcast
        if (h.tag == MSG_ALL_REDUCTION_DOWN) {
            // forward to children
            if (getLeftChildRank(_my_rank) < _comm_size)
                MyMpi::isendCopy(getLeftChildRank(_my_rank), MSG_ALL_REDUCTION_DOWN, data);
            if (getRightChildRank(_my_rank) < _comm_size)
                MyMpi::isendCopy(getRightChildRank(_my_rank), MSG_ALL_REDUCTION_DOWN, data);
            state.cbResult(elem); // publish result
            _states_by_id.erase(id); // clean up
        }
    }

private:
    void forward(AllReduceState& state) {

        // Prepare serialized data, followed by reduction ID
        auto packed = state.contrib.serialize();
        auto sizeBefore = packed.size();
        packed.resize(sizeBefore + sizeof(unsigned long));
        memcpy(packed.data() + sizeBefore, &state.id, sizeof(unsigned long));

        if (_my_rank == 0) {
            // Root: switch to broadcast via a self message
            MyMpi::isend(_my_rank, MSG_ALL_REDUCTION_DOWN, std::move(packed));
        } else {
            // aggregate upwards
            MyMpi::isend(getParentRank(_my_rank), MSG_ALL_REDUCTION_UP, std::move(packed));
        }
        state.numArrivedContribs = 0;
    }

    static int getLeftChildRank(int rank) {return 2*rank+1;}
    static int getRightChildRank(int rank) {return 2*rank+2;}
    static int getParentRank(int rank) {return (rank-1)/2;}

};
