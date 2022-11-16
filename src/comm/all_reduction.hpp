
#pragma once

#include "data/reduceable.hpp"
#include "mympi.hpp"
#include "util/tsl/robin_map.h"

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

    int _comm_size;
    int _my_rank;
    int _parent_rank;
    int _left_child_rank;
    int _right_child_rank;

    int _num_desired_contribs;

    // An ID is assigned to each all-reduction call.
    struct AllReduceState {
        int id;
        std::function<void(T&)> cbResult;
        T contribSelf;
        T contribLeft;
        T contribRight;
        int numArrivedContribs {0};
    };
    // Maintain a "state" struct for each active all-reduction ID.
    tsl::robin_map<int, AllReduceState> _states_by_id;

public:
    AllReduction<T>(MPI_Comm comm, MessageQueue& msgQ, int instanceId) : 
            _comm(comm), _msg_q(msgQ), _instance_id(instanceId) {

        _my_rank = MyMpi::rank(_comm);
        _comm_size = MyMpi::size(_comm);
        setUpCommunicationTree();

        // How many contributions should this process expect?
        _num_desired_contribs = 1; // yourself
        if (_left_child_rank >= 0) _num_desired_contribs++;
        if (_right_child_rank >= 0) _num_desired_contribs++;

        _ref_up = msgQ.registerCallback(MSG_ALL_REDUCTION_UP, [&](auto& h) {handle(h);});
        _ref_down = msgQ.registerCallback(MSG_ALL_REDUCTION_DOWN, [&](auto& h) {handle(h);});
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
        state.contribSelf = contribution;
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
            (isFromLeftChild(h.source) ? state.contribLeft : state.contribRight) = elem;
            state.numArrivedContribs++;
            if (state.numArrivedContribs == _num_desired_contribs) forward(state);
        }
        
        // Broadcast
        if (h.tag == MSG_ALL_REDUCTION_DOWN) {
            // forward to children
            if (_left_child_rank >= 0)
                MyMpi::isendCopy(_left_child_rank, MSG_ALL_REDUCTION_DOWN, data);
            if (_right_child_rank >= 0)
                MyMpi::isendCopy(_right_child_rank, MSG_ALL_REDUCTION_DOWN, data);
            state.cbResult(elem); // publish result
            _states_by_id.erase(callId); // clean up
        }
    }

private:

    // Defines _parent_rank, _left_child_rank, and _right_child_rank in such a way
    // that the communication structure is an in-order binary tree.
    void setUpCommunicationTree() {

        int parentRank = -1, leftChildRank = -1, rightChildRank = -1;

        // - compute offset based the node's depth
        int power = 1;
        while ((_my_rank+1) % (2*power) == 0) {
            power *= 2;
        }
        // Parent rank
        if (_my_rank % (4*power) >= 2*power || _my_rank+power >= _comm_size) {
            parentRank = _my_rank - power;
        } else {
            parentRank = _my_rank + power;
        }
        // Child ranks
        if (power > 1) {
            power /= 2;
            // Left child
            leftChildRank = _my_rank - power;
            // Right child
            if (_my_rank+1 < _comm_size) {
                rightChildRank = _my_rank + power;
                while (rightChildRank >= _comm_size) {
                    // Find the first transitive child which is valid
                    power /= 2;
                    rightChildRank -= power;
                }
            }
        }

        // Create a mapping from the communicator's ranks to world ranks
        // (since MessageQueue works with global ranks exclusively)
        MPI_Group groupComm; MPI_Comm_group(_comm, &groupComm);
        MPI_Group groupWorld; MPI_Comm_group(MPI_COMM_WORLD, &groupWorld);
        std::vector<int> localRanks = {_my_rank, std::max(0,parentRank), std::max(0,leftChildRank), std::max(0,rightChildRank)};
        std::vector<int> worldRanks(localRanks.size());
        MPI_Group_translate_ranks(groupComm, localRanks.size(), localRanks.data(), 
            groupWorld, worldRanks.data());
        _my_rank = worldRanks[0];
        _parent_rank = parentRank == -1 ? -1 : worldRanks[1];
        _left_child_rank = leftChildRank == -1 ? -1 : worldRanks[2];
        _right_child_rank = rightChildRank == -1 ? -1 : worldRanks[3];

        LOG(V2_INFO, "TREE rank=%i parent=%i left=%i right=%i\n", 
            _my_rank, _parent_rank, _left_child_rank, _right_child_rank);
    }

    bool isFromLeftChild(int worldRank) {
        MPI_Group groupComm; MPI_Comm_group(_comm, &groupComm);
        MPI_Group groupWorld; MPI_Comm_group(MPI_COMM_WORLD, &groupWorld);
        int worldRanks[2] = {_my_rank, worldRank};
        int localRanks[2] = {-1, -1};
        MPI_Group_translate_ranks(groupWorld, 2, worldRanks, groupComm, localRanks);
        return localRanks[1] < localRanks[0];
    }

    void forward(AllReduceState& state) {

        // Perform the aggregation respecting the correct ordering of elements
        // since the aggregate operation is not required to be commutative
        if (state.numArrivedContribs == 3) {
            state.contribSelf.aggregate(state.contribRight);
        }
        if (state.numArrivedContribs >= 2) {
            state.contribLeft.aggregate(state.contribSelf);
            state.contribSelf = std::move(state.contribLeft);
        }
        T& aggregation = state.contribSelf;

        // Prepare serialized data, followed by reduction ID
        auto packed = aggregation.serialize();
        auto sizeBefore = packed.size();
        packed.resize(sizeBefore + 2*sizeof(int));
        memcpy(packed.data() + sizeBefore, &_instance_id, sizeof(int));
        memcpy(packed.data() + sizeBefore + sizeof(int), &state.id, sizeof(int));

        if (_parent_rank < 0) {
            // Root: switch to broadcast via a self message
            MyMpi::isend(_my_rank, MSG_ALL_REDUCTION_DOWN, std::move(packed));
        } else {
            // aggregate upwards
            MyMpi::isend(_parent_rank, MSG_ALL_REDUCTION_UP, std::move(packed));
        }
        state.numArrivedContribs = 0;
    }
};
