
#pragma once

#include <limits>

#include "data/reduceable.hpp"
#include "mympi.hpp"
#include "util/tsl/robin_map.h"
#include "util/sys/timer.hpp"
#include "msg_queue/message_subscription.hpp"

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
    std::list<MessageSubscription> _subscriptions;
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
        PREFIXSUM_INCL_EXCL_TOTAL,
        SPARSE_PREFIXSUM_INCL_EXCL_TOTAL
    };

    // Callback definition for returning a result
    typedef std::function<void(std::list<T>&)> ResultCallback;

    // Serializable struct for an ID-qualified list of reduceables
    struct ReduceableList : public Serializable {
        int instanceId {-1};
        int callId {-1};
        int contributionId {0};
        int resultCounter {0};
        std::list<T> items;
        virtual std::vector<uint8_t> serialize() const override {
            std::vector<uint8_t> packed(4*sizeof(int));
            memcpy(packed.data(), &instanceId, sizeof(int));
            memcpy(packed.data()+sizeof(int), &callId, sizeof(int));
            memcpy(packed.data()+2*sizeof(int), &contributionId, sizeof(int));
            memcpy(packed.data()+3*sizeof(int), &resultCounter, sizeof(int));
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
            memcpy(&contributionId, packed.data()+i, sizeof(int)); i += sizeof(int);
            memcpy(&resultCounter, packed.data()+i, sizeof(int)); i += sizeof(int);
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
    struct OperationState {
        int id; // call ID
        Mode mode = ALLREDUCE; // operation mode
        ResultCallback cbResult;
        T contribSelf;
        T contribLeft;
        T contribRight;
        T aggregation;
        int numArrivedContribs {0};
        T& aggregateContributions() {
            if (numArrivedContribs >= 2)
                aggregation.aggregate(contribLeft);
            aggregation.aggregate(contribSelf);
            if (numArrivedContribs == 3)
                aggregation.aggregate(contribRight);
            numArrivedContribs = 0;
            return aggregation;
        }
    };
    // Maintain a "state" struct for each ongoing operation
    tsl::robin_map<int, OperationState> _states_by_id;

    struct SparseOperationBundle {
        T contribSelf;
        int contribIdSelf {0};
        T contribLeft;
        int contribIdLeft {0};
        T contribRight;
        int contribIdRight {0};
        T aggregation;
        T& aggregateContributions() {
            aggregation = T();
            if (contribIdLeft != 0)
                aggregation.aggregate(contribLeft);
            if (contribIdSelf != 0)
                aggregation.aggregate(contribSelf);
            if (contribIdRight != 0)
                aggregation.aggregate(contribRight);
            return aggregation;
        }
        bool hasContributions() const {
            return getNumContributions() > 0;
        }
        int getNumContributions() const {
            int num = 0;
            if (contribIdLeft != 0) num++;
            if (contribIdSelf != 0) num++;
            if (contribIdRight != 0) num++;
            return num;
        }
    };
    struct SparseOperationState {
        int id; // call ID
        ResultCallback cbResult;
        bool differential;
        int contributionIdCounter = 1; // the ID of the contribution *currently in preparation*
        tsl::robin_map<int, SparseOperationBundle> bundlesByContribId;
        float delaySeconds {0};
        float lastForwardTime {0};
    };
    std::vector<int> _sparse_state_ids;
    tsl::robin_map<int, SparseOperationState> _sparse_states_by_id;

    // Counts how many result broadcasts this instance already received
    int _result_counter {0};

public:
    // @param comm The MPI communicator within which collective operations should be
    // performed. The order in which data will be aggregated is equivalent to the
    // ranking of MPI processes in comm.
    // @param msqQ The message queue which distributes incoming messages.
    // @param instanceId The ID of this instance across all participating processes. 
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
        auto tags = {MSG_ASYNC_COLLECTIVE_UP, MSG_ASYNC_COLLECTIVE_DOWN, 
            MSG_ASYNC_SPARSE_COLLECTIVE_UP, MSG_ASYNC_SPARSE_COLLECTIVE_DOWN};        
        for (int tag : tags) {
            _subscriptions.emplace_back(tag, [&](auto& h) {handle(h);});
        }
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

    // Begin a new sparse prefix sum.
    // @param callId the ID of this collective operation call
    // @param delaySeconds the minimum time to wait in between forwarding contributions
    // @param callbackOnResult the function which is called whenever a new result
    // has been received
    void initializeSparsePrefixSum(int callId, float delaySeconds, ResultCallback callbackOnResult) {
        auto& state = _sparse_states_by_id[callId];
        state.id = callId;
        state.cbResult = callbackOnResult;
        state.differential = false;
        state.delaySeconds = delaySeconds;
    }

    // Begin a new sparse prefix sum.
    // @param callId the ID of this collective operation call
    // @param delaySeconds the minimum time to wait in between forwarding contributions
    // @param callbackOnResult the function which is called whenever a new result
    // has been received
    void initializeDifferentialSparsePrefixSum(int callId, float delaySeconds, 
            ResultCallback callbackOnResult) {

        auto& state = _sparse_states_by_id[callId];
        state.id = callId;
        state.cbResult = callbackOnResult;
        state.differential = true;
        state.delaySeconds = delaySeconds;
    }

    // Contribute an element to an ongoing sparse prefix sum.
    void contributeToSparsePrefixSum(int callId, const T& contribution) {
        auto& state = _sparse_states_by_id[callId];
        
        int contribId = state.contributionIdCounter;
        while (state.bundlesByContribId[contribId].contribIdSelf != 0)
            contribId++;
        
        auto& bundle = state.bundlesByContribId[contribId];
        bundle.contribIdSelf = contribId;
        bundle.contribSelf = contribution;
    }

    // Must be called periodically to advance sparse operations.
    void advanceSparseOperations() {

        auto time = Timer::elapsedSecondsCached();

        // Look for an operation which can be advanced
        for (auto it = _sparse_states_by_id.begin(); it != _sparse_states_by_id.end(); ++it) {
            auto callId = it->first;
            auto& state = _sparse_states_by_id.at(callId);

            {
                // Bundle which is about to be sent next
                auto& bundle = state.bundlesByContribId[state.contributionIdCounter];

                // Is the current bundle ready to be forwarded?
                if (!bundle.hasContributions())
                    continue; // -- no: no contributions yet
                if (time - state.lastForwardTime < state.delaySeconds)
                    continue; // -- no: not enough time passed waiting for remaining contribs
                // -- yes!
            }
            
            if (state.differential) {
                // Update bundle with data from previous bundle
                auto& prevBundle = state.bundlesByContribId[state.contributionIdCounter-1];
                // Re-fetch reference since the other bundle could have just been created
                // and there may be no reference stability
                auto& bundle = state.bundlesByContribId[state.contributionIdCounter];
                if (bundle.contribIdLeft == 0 && prevBundle.contribIdLeft != 0) {
                    bundle.contribIdLeft = prevBundle.contribIdLeft;
                    bundle.contribLeft = prevBundle.contribLeft;
                }
                if (bundle.contribIdSelf == 0 && prevBundle.contribIdSelf != 0) {
                    bundle.contribIdSelf = prevBundle.contribIdSelf;
                    bundle.contribSelf = prevBundle.contribSelf;
                }
                if (bundle.contribIdRight == 0 && prevBundle.contribIdRight != 0) {
                    bundle.contribIdRight = prevBundle.contribIdRight;
                    bundle.contribRight = prevBundle.contribRight;
                }
            } 

            // Send bundle with aggregation of all present contributions
            forward(callId, SPARSE_PREFIXSUM_INCL_EXCL_TOTAL,
                state.bundlesByContribId[state.contributionIdCounter].aggregateContributions(), 
                state.contributionIdCounter);
            // Proceed with next bundle next time
            state.contributionIdCounter++;
            state.lastForwardTime = time;
        }
    }

    int getNumReceivedResults() const {
        return _result_counter;
    }

private:
    void initOp(Mode mode, int callId, const T& contribution, ResultCallback callbackOnResult) {
        auto& state = initState(mode, callId, contribution);
        state.cbResult = callbackOnResult;
        if (state.numArrivedContribs == _num_desired_contribs)
            forward(callId, mode, state.aggregateContributions());
    }

    OperationState& initState(Mode mode, int callId, const T& contribution) {
        auto& state = _states_by_id[callId];
        state.id = callId;
        state.mode = mode;
        state.contribSelf = contribution;
        state.numArrivedContribs++;
        return state;
    }

    // MessageHandles of tags MSG_ALL_REDUCTION_{UP,DOWN} are routed to here.
    void handle(MessageHandle& h) {

        if (h.tag == MSG_ASYNC_COLLECTIVE_UP || h.tag == MSG_ASYNC_COLLECTIVE_DOWN) {

            // Deserialize data
            auto data = Serializable::get<ReduceableList>(h.getRecvData());
            if (data.instanceId != _instance_id) return; // matching instance ID?
            // Retrieve local state of the associated call
            auto& state = _states_by_id[data.callId];
            state.id = data.callId;

            // Reduction
            if (h.tag == MSG_ASYNC_COLLECTIVE_UP) {
                (isFromLeftChild(h.source) ? state.contribLeft : state.contribRight) = data.items.front();
                state.numArrivedContribs++;
                if (state.numArrivedContribs == _num_desired_contribs)
                    forward(state.id, state.mode, state.aggregateContributions());
            }
            
            // Broadcast
            if (h.tag == MSG_ASYNC_COLLECTIVE_DOWN) {
                auto resultList = broadcastAndDigest(state.mode, data, state.contribLeft, state.contribSelf);
                state.cbResult(resultList); // publish result locally
                _states_by_id.erase(data.callId); // clean up
            }
        }

        if (h.tag == MSG_ASYNC_SPARSE_COLLECTIVE_UP || h.tag == MSG_ASYNC_SPARSE_COLLECTIVE_DOWN) {

            // Deserialize data
            auto data = Serializable::get<ReduceableList>(h.getRecvData());
            if (data.instanceId != _instance_id) return; // matching instance ID?
            // Retrieve local state of the associated call
            auto& state = _sparse_states_by_id[data.callId];
            state.id = data.callId;

            // Reduction
            if (h.tag == MSG_ASYNC_SPARSE_COLLECTIVE_UP) {
                // Find 1st unsent bundle with a free "slot" for this child
                bool fromLeftChild = isFromLeftChild(h.source);
                int contribId = state.contributionIdCounter;
                while (fromLeftChild && state.bundlesByContribId[contribId].contribIdLeft != 0)
                    contribId++;
                while (!fromLeftChild && state.bundlesByContribId[contribId].contribIdRight != 0)
                    contribId++;
                
                auto& bundle = state.bundlesByContribId[contribId];
                LOG(V6_DEBGV, "SPARSE got contribution ID=%i from [%i]; adding to my contrib. ID %i (current ID: %i)\n", 
                    data.contributionId, h.source, contribId, state.contributionIdCounter);
                // Store received data in the found bundle
                (fromLeftChild ? bundle.contribLeft : bundle.contribRight) = data.items.front();
                (fromLeftChild ? bundle.contribIdLeft : bundle.contribIdRight) = data.contributionId;
                // Perhaps the operation can be advanced now
                advanceSparseOperations();
            }

            // Broadcast
            if (h.tag == MSG_ASYNC_SPARSE_COLLECTIVE_DOWN) {
                std::list<T> resultList;
                int contribId = data.contributionId;
                if (contribId == 0) {
                    // Did not contribute to this broadcast:
                    // Just forward everything, also with contribution ID 0
                    LOG(V6_DEBGV, "SPARSE got broadcast with no personal contribution from [%i]\n", h.source);
                    T emptyContrib;
                    resultList = broadcastAndDigest(SPARSE_PREFIXSUM_INCL_EXCL_TOTAL, data, emptyContrib, emptyContrib);
                } else {
                    // DID contribute to this broadcast
                    LOG(V6_DEBGV, "SPARSE got broadcast with contribution ID %i from [%i]\n", contribId, h.source);
                    auto& bundle = state.bundlesByContribId[contribId];
                    resultList = broadcastAndDigest(SPARSE_PREFIXSUM_INCL_EXCL_TOTAL, data, bundle.contribLeft, bundle.contribSelf,
                        /*leftContribId=*/bundle.contribIdLeft, /*rightContribId=*/bundle.contribIdRight);
                }
                state.cbResult(resultList); // publish result locally
                if (contribId != 0) {
                    LOG(V6_DEBGV, "SPARSE remove bundle with contribution ID %i\n", contribId);
                    state.bundlesByContribId.erase(contribId-2); // clean up previous-previous contribution
                }
            }

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

    void forward(int callId, Mode mode, T& aggregation, int contributionId = 0) {

        std::vector<uint8_t> packed;
        if (_parent_rank < 0) {
            // Root: switch to broadcast via a self message
            _result_counter++;
            int msgTag = mode == SPARSE_PREFIXSUM_INCL_EXCL_TOTAL ? 
                MSG_ASYNC_SPARSE_COLLECTIVE_DOWN : MSG_ASYNC_COLLECTIVE_DOWN;
            if (mode == ALLREDUCE) {
                // Forward aggregated element
                packed = serialize(callId, aggregation);
            } else if (mode == PREFIXSUM_INCL_EXCL_TOTAL || mode == SPARSE_PREFIXSUM_INCL_EXCL_TOTAL) {
                // Forward neutral AND aggregated element
                T neutralElem;
                packed = serialize(callId, neutralElem, aggregation, contributionId);
            } else {
                // Forward neutral element
                T neutralElem;
                packed = serialize(callId, neutralElem);
            }
            MyMpi::isend(_my_rank, msgTag, std::move(packed));
        } else {
            int msgTag = mode == SPARSE_PREFIXSUM_INCL_EXCL_TOTAL ? 
                MSG_ASYNC_SPARSE_COLLECTIVE_UP : MSG_ASYNC_COLLECTIVE_UP;
            // aggregate upwards
            packed = serialize(callId, aggregation, contributionId);
            MyMpi::isend(_parent_rank, msgTag, std::move(packed));
        }
    }

    std::list<T> broadcastAndDigest(Mode mode, ReduceableList& data, T& contribLeft, T& contribSelf,
            int leftContribId = 0, int rightContribId = 0) {

        std::list<T> resultList;
        auto& elem = data.items.front();
        _result_counter = data.resultCounter;

        if (mode == ALLREDUCE) {

            // AllReduction: just forward received data to children
            if (_left_child_rank >= 0)
                MyMpi::isend(_left_child_rank, MSG_ASYNC_COLLECTIVE_DOWN, data);
            if (_right_child_rank >= 0)
                MyMpi::isend(_right_child_rank, MSG_ASYNC_COLLECTIVE_DOWN, data);
            // Store first and only deserialized item
            resultList.push_back(std::move(elem));

        } else {

            // Prefix sum.
            int msgTag = mode == SPARSE_PREFIXSUM_INCL_EXCL_TOTAL ? 
                MSG_ASYNC_SPARSE_COLLECTIVE_DOWN : MSG_ASYNC_COLLECTIVE_DOWN;
            // Send received data to left child and aggregate data with left child's data
            if (_left_child_rank >= 0) {
                data.contributionId = leftContribId;
                MyMpi::isend(_left_child_rank, msgTag, data);
                elem.aggregate(contribLeft);
            }
            // Store exclusive result
            if (mode != PREFIXSUM_INCL) {
                resultList.push_back(elem);
            }
            // Aggregate data with your own data
            elem.aggregate(contribSelf);
            // Send inclusive result to right child
            if (_right_child_rank >= 0) {
                auto packed = mode == PREFIXSUM_INCL_EXCL_TOTAL || mode == SPARSE_PREFIXSUM_INCL_EXCL_TOTAL ? 
                    serialize(data.callId, elem, data.items.back(), rightContribId) : 
                    serialize(data.callId, elem, rightContribId);
                MyMpi::isend(_right_child_rank, msgTag, std::move(packed));
            }
            // Store inclusive result
            if (mode != PREFIXSUM_EXCL) {
                resultList.push_back(std::move(elem));
            }
            // Store total result
            if (mode == PREFIXSUM_INCL_EXCL_TOTAL || mode == SPARSE_PREFIXSUM_INCL_EXCL_TOTAL) {
                resultList.push_back(std::move(data.items.back()));
            }
        }

        return resultList;
    }

    std::vector<uint8_t> serialize(int callId, T& elem, int contributionId = 0) {
        ReduceableList data;
        data.instanceId = _instance_id;
        data.callId = callId;
        data.contributionId = contributionId;
        data.resultCounter = _result_counter;
        data.items.push_back(elem);
        return data.serialize();
    }
    std::vector<uint8_t> serialize(int callId, T& elem1, T& elem2, int contributionId = 0) {
        ReduceableList data;
        data.instanceId = _instance_id;
        data.callId = callId;
        data.contributionId = contributionId;
        data.resultCounter = _result_counter;
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

        LOG(V6_DEBGV, "TREE rank=%i parent=%i left=%i right=%i\n", 
            _my_rank, _parent_rank, _left_child_rank, _right_child_rank);
    }
};
