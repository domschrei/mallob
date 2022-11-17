
#pragma once

#include "data/reduceable.hpp"
#include "mympi.hpp"
#include "util/tsl/robin_map.h"

// T must be a subclass of Reduceable (see data/reduceable.hpp).
// An arbitrary number of collective operations can be performed with a single instance of this class.
// These operations can also be performed concurrently.
// In addition, each MPI process can hold multiple instances of this class.
// This flexibility has the trade-off that constructing an instance of this class requires
// an *instance ID* which must be the same across all MPI processes in order to participate
// in the same collective operation. In addition, each operation must be tagged with a *call ID*
// which likewise must be the same across all MPI processes participating in the operation.
template <class T>
class AsyncCollective {

private:
    MPI_Comm _comm;
    MessageQueue& _msg_q;
    // References for the message callbacks which need to be deleted at destruction
    MessageQueue::CallbackRef _ref_up;
    MessageQueue::CallbackRef _ref_down;
    // ID which all corresponding AsyncCollective instances across the MPI processes share
    int _instance_id;

    // Communicator data
    int _comm_size;
    int _my_rank;
    int _parent_rank;
    int _left_child_rank;
    int _right_child_rank;

    // How many contributions to wait for until data is forwarded?
    int _num_desired_contribs;

    // Internal distinction of operation modes
    enum Mode {
        ALLREDUCE, 
        PREFIXSUM_INCL, 
        PREFIXSUM_EXCL, 
        PREFIXSUM_INCL_EXCL,
        PREFIXSUM_INCL_EXCL_TOTAL
    };

    // Callback definition for returning a result
    typedef std::function<void(std::list<T>&)> ResultCallback;

    // Serializable struct for an ID-qualified list of reduceables
    struct ReduceableList : public Serializable {
        int instanceId;
        int callId;
        std::list<T> items;
        virtual std::vector<uint8_t> serialize() const override {
            std::vector<uint8_t> packed(2*sizeof(int));
            memcpy(packed.data(), &instanceId, sizeof(int));
            memcpy(packed.data()+sizeof(int), &callId, sizeof(int));
            int packedSize = packed.size();
            for (auto& item : items) {
                auto serializedItem = item.serialize();
                int itemSize = serializedItem.size();
                packed.resize(packedSize + sizeof(int) + itemSize);
                memcpy(packed.data() + packedSize, &itemSize, sizeof(int));
                memcpy(packed.data() + packedSize + sizeof(int), serializedItem.data(), itemSize);
                packedSize = packed.size();
            }
            return packed;
        }
        virtual ReduceableList& deserialize(const std::vector<uint8_t>& packed) override {
            items.clear();
            size_t i = 0;
            memcpy(&instanceId, packed.data()+i, sizeof(int)); i += sizeof(int);
            memcpy(&callId, packed.data()+i, sizeof(int)); i += sizeof(int);
            while (i < packed.size()) {
                int itemSize;
                memcpy(&itemSize, packed.data()+i, sizeof(int)); i += sizeof(int);
                std::vector<uint8_t> serializedItem(itemSize);
                memcpy(serializedItem.data(), packed.data()+i, itemSize); i += itemSize;
                items.push_back(Serializable::get<T>(serializedItem));
            }
            return *this;
        }
    };

    // Internal state for each ongoing collective operation
    struct AllReduceState {
        int id; // call ID
        Mode mode = ALLREDUCE; // operation mode
        ResultCallback cbResult;
        T contribSelf;
        T contribLeft;
        T contribRight;
        T aggregation;
        int numArrivedContribs {0};
    };
    // Maintain a "state" struct for each ongoing operation
    tsl::robin_map<int, AllReduceState> _states_by_id;

public:
    AsyncCollective<T>(MPI_Comm comm, MessageQueue& msgQ, int instanceId) : 
            _comm(comm), _msg_q(msgQ), _instance_id(instanceId) {

        // Initialize communication structure
        _my_rank = MyMpi::rank(_comm);
        _comm_size = MyMpi::size(_comm);
        setUpCommunicationTree();

        // How many contributions should this process expect?
        _num_desired_contribs = 1; // yourself
        if (_left_child_rank >= 0) _num_desired_contribs++;
        if (_right_child_rank >= 0) _num_desired_contribs++;

        // Register callbacks in message queue
        _ref_up = msgQ.registerCallback(MSG_ALL_REDUCTION_UP, [&](auto& h) {handle(h);});
        _ref_down = msgQ.registerCallback(MSG_ALL_REDUCTION_DOWN, [&](auto& h) {handle(h);});
    }

    ~AsyncCollective() {
        // Clear callbacks in message queue (to avoid "dangling lambdas")
        _msg_q.clearCallback(MSG_ALL_REDUCTION_UP, _ref_up);
        _msg_q.clearCallback(MSG_ALL_REDUCTION_DOWN, _ref_down);
    }

    // Begin a new all-reduction with the provided local contribution
    // and a callback which will be called when the all-reduction is finished.
    // The callback will be called from the main thread and its argument is
    // a list of size one with the desired result as its only element.
    void allReduce(int callId, const T& contribution, ResultCallback callbackOnResult) {
        initOp(ALLREDUCE, callId, contribution, callbackOnResult);
    }

    // Begin a new prefix sum with the provided local contribution
    // and a callback which will be called when the prefix sum is finished.
    // The callback will be called from the main thread and its argument is
    // a list of size one with the incluside prefix sum as its only element.
    void inclusivePrefixSum(int callId, const T& contribution, ResultCallback callbackOnResult) {
        initOp(PREFIXSUM_INCL, callId, contribution, callbackOnResult);
    }

    // Begin a new prefix sum with the provided local contribution
    // and a callback which will be called when the prefix sum is finished.
    // The callback will be called from the main thread and its argument is
    // a list of size one with the excluside prefix sum as its only element.
    void exclusivePrefixSum(int callId, const T& contribution, ResultCallback callbackOnResult) {
        initOp(PREFIXSUM_EXCL, callId, contribution, callbackOnResult);
    }

    // Begin a new prefix sum with the provided local contribution
    // and a callback which will be called when the prefix sum is finished.
    // The callback will be called from the main thread and its argument is
    // a list of size two with the exclusive prefix sum at the front and the
    // incluside prefix sum at the end.
    void inclAndExclPrefixSum(int callId, const T& contribution, ResultCallback callbackOnResult) {
        initOp(PREFIXSUM_INCL_EXCL, callId, contribution, callbackOnResult);
    }

    // Begin a new prefix sum with the provided local contribution
    // and a callback which will be called when the prefix sum is finished.
    // The callback will be called from the main thread and its argument is
    // a list of size three containing the exclusive prefix sum, the inclusive
    // prefix sum, and the total sum (in this order).
    void inclAndExclPrefixSumWithTotal(int callId, const T& contribution, ResultCallback callbackOnResult) {
        initOp(PREFIXSUM_INCL_EXCL_TOTAL, callId, contribution, callbackOnResult);
    }

private:
    void initOp(Mode mode, int callId, const T& contribution, ResultCallback callbackOnResult) {
        auto& state = initState(mode, callId, contribution);
        state.cbResult = callbackOnResult;
        if (state.numArrivedContribs == _num_desired_contribs) forward(state);
    }

    AllReduceState& initState(Mode mode, int callId, const T& contribution) {
        auto& state = _states_by_id[callId];
        state.id = callId;
        state.mode = mode;
        state.contribSelf = contribution;
        state.numArrivedContribs++;
        return state;
    }

    // MessageHandles of tags MSG_ALL_REDUCTION_{UP,DOWN} are routed to here.
    void handle(MessageHandle& h) {

        if (h.tag != MSG_ALL_REDUCTION_UP && h.tag != MSG_ALL_REDUCTION_DOWN) return;

        // Deserialize data
        auto data = Serializable::get<ReduceableList>(h.getRecvData());
        if (data.instanceId != _instance_id) return; // correct instance ID?
        
        // Retrieve local state of the associated call
        auto& state = _states_by_id[data.callId];
        state.id = data.callId;

        // Reduction
        if (h.tag == MSG_ALL_REDUCTION_UP) {
            (isFromLeftChild(h.source) ? state.contribLeft : state.contribRight) = data.items.front();
            state.numArrivedContribs++;
            if (state.numArrivedContribs == _num_desired_contribs) forward(state);
        }
        
        // Broadcast
        if (h.tag == MSG_ALL_REDUCTION_DOWN) {
            std::list<T> resultList;
            auto& elem = data.items.front();
            if (state.mode == ALLREDUCE) {
                // just forward to children
                if (_left_child_rank >= 0)
                    MyMpi::isendCopy(_left_child_rank, MSG_ALL_REDUCTION_DOWN, h.getRecvData());
                if (_right_child_rank >= 0)
                    MyMpi::isendCopy(_right_child_rank, MSG_ALL_REDUCTION_DOWN, h.getRecvData());
                // Store first and only deserialized item
                resultList.push_back(std::move(elem));
            } else {
                // Send received data to left child and aggregate data with left child's data
                if (_left_child_rank >= 0) {
                    MyMpi::isendCopy(_left_child_rank, MSG_ALL_REDUCTION_DOWN, h.getRecvData());
                    elem.aggregate(state.contribLeft);
                }
                // Store exclusive result
                if (state.mode == PREFIXSUM_EXCL || state.mode == PREFIXSUM_INCL_EXCL 
                        || state.mode == PREFIXSUM_INCL_EXCL_TOTAL) {
                    resultList.push_back(elem);
                }
                // Aggregate data with your own data
                elem.aggregate(state.contribSelf);
                // Send inclusive result to right child
                if (_right_child_rank >= 0) {
                    auto packed = state.mode == PREFIXSUM_INCL_EXCL_TOTAL ? 
                        serialize(data.callId, elem, data.items.back()) : serialize(data.callId, elem);
                    MyMpi::isend(_right_child_rank, MSG_ALL_REDUCTION_DOWN, std::move(packed));
                }
                // Store inclusive result
                if (state.mode == PREFIXSUM_INCL || state.mode == PREFIXSUM_INCL_EXCL 
                        || state.mode == PREFIXSUM_INCL_EXCL_TOTAL) {
                    resultList.push_back(std::move(elem));
                }
                // Store total result
                if (state.mode == PREFIXSUM_INCL_EXCL_TOTAL) {
                    resultList.push_back(std::move(data.items.back()));
                }
            }
            state.cbResult(resultList); // publish result
            _states_by_id.erase(data.callId); // clean up
        }
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
        if (state.numArrivedContribs >= 2) {
            state.aggregation.aggregate(state.contribLeft);
        }
        state.aggregation.aggregate(state.contribSelf);
        if (state.numArrivedContribs == 3) {
            state.aggregation.aggregate(state.contribRight);
        }
        T& aggregation = state.aggregation;

        std::vector<uint8_t> packed;
        if (_parent_rank < 0) {
            // Root: switch to broadcast via a self message
            if (state.mode == ALLREDUCE) {
                // Forward aggregated element
                packed = serialize(state.id, aggregation);
            } else if (state.mode == PREFIXSUM_INCL_EXCL_TOTAL) {
                // Forward neutral AND aggregated element
                T neutralElem;
                packed = serialize(state.id, neutralElem, aggregation);
            } else {
                // Forward neutral element
                T neutralElem;
                packed = serialize(state.id, neutralElem);
            }
            MyMpi::isend(_my_rank, MSG_ALL_REDUCTION_DOWN, std::move(packed));
        } else {
            // aggregate upwards
            packed = serialize(state.id, aggregation);
            MyMpi::isend(_parent_rank, MSG_ALL_REDUCTION_UP, std::move(packed));
        }
        state.numArrivedContribs = 0;
    }

    std::vector<uint8_t> serialize(int callId, T& elem) {
        ReduceableList data;
        data.instanceId = _instance_id;
        data.callId = callId;
        data.items.push_back(elem);
        return data.serialize();
    }
    std::vector<uint8_t> serialize(int callId, T& elem1, T& elem2) {
        ReduceableList data;
        data.instanceId = _instance_id;
        data.callId = callId;
        data.items.push_back(elem1);
        data.items.push_back(elem2);
        return data.serialize();
    }

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
};
