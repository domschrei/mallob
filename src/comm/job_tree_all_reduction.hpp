
#pragma once

#include <list>
#include <set>

#include "comm/job_tree_snapshot.hpp"
#include "comm/msg_queue/cond_message_subscription.hpp"
#include "comm/msgtags.h"
#include "data/serializable.hpp"
#include "util/logger.hpp"
#include "util/sys/thread_pool.hpp"
#include "data/job_transfer.hpp"

#define VERB_ALLRED V5_DEBG

class JobTreeAllReduction {

public:
    typedef std::vector<int> AllReduceElement;

private:
    JobTreeSnapshot _tree;
    JobMessage _base_msg;
    AllReduceElement _neutral_elem;

    std::optional<AllReduceElement> _local_elem;

    // Sort arrived child elems by source rank
    // in order to render aggregation deterministic
    struct ChildElemPair {
        int source;
        AllReduceElement elem;
        bool operator<(const ChildElemPair& other) const {
            return source < other.source;
        }
    };
    std::set<ChildElemPair> _child_elems;
    int _num_expected_child_elems;
    IntPair _expected_child_ranks;
    IntPair _expected_child_indices;
    std::pair<ctx_id_t, ctx_id_t> _expected_child_ctx_ids;
    std::pair<bool, bool> _received_child_elems;

    bool _is_root;
    int _parent_rank;
    int _parent_index;
    ctx_id_t _parent_ctx_id;

    bool _aggregating = false;
    bool _have_unanswered_returnToSender=false;
    std::vector<int> _returnToSender_payload{};
    int _returnToSender_counter=0;
    std::future<void> _future_aggregate;
    std::function<AllReduceElement(std::list<AllReduceElement>&)> _aggregator;
    std::optional<AllReduceElement> _aggregated_elem;

    bool _has_transformation_at_root = false;
    std::function<AllReduceElement(const AllReduceElement&)> _transformation_at_root;

    bool _has_inplace_transformation_at_root = false;
    std::function<void(AllReduceElement&)> _inplace_transformation_at_root;

    bool _contributed = false;
    bool _reduction_locally_done = false;
    bool _finished = false;
    bool _valid = true;
    bool _broadcast_enabled = true;

    CondMessageSubscription _sub_aggregate;
    CondMessageSubscription _sub_broadcast;

public:
    JobTreeAllReduction(const JobTreeSnapshot& tree, JobMessage baseMsg, AllReduceElement&& neutralElem, 
            std::function<AllReduceElement(std::list<AllReduceElement>&)> aggregator) :
        _tree(tree), _base_msg(baseMsg), _neutral_elem(std::move(neutralElem)),
        _num_expected_child_elems(_tree.nbChildren), _aggregator(aggregator),
        _sub_aggregate(MSG_JOB_TREE_MODULAR_REDUCE, [this](MessageHandle& h) {
            JobMessage msg = Serializable::get<JobMessage>(h.getRecvData());
            return receive(h.source, h.tag, msg);
        }), _sub_broadcast(MSG_JOB_TREE_MODULAR_BROADCAST, [this](MessageHandle& h) {
            LOG(V4_VVER, "BROADCAST\n");
            JobMessage msg = Serializable::get<JobMessage>(h.getRecvData());
            return receive(h.source, h.tag, msg);
        }) {

        int leftRank = _tree.leftChildNodeRank;
        int rightRank = _tree.rightChildNodeRank;
        _expected_child_ranks = IntPair(leftRank, rightRank);
        _expected_child_indices = IntPair(
            leftRank<0?-1: _tree.leftChildIndex,
            rightRank<0?-1: _tree.rightChildIndex
        );
        _expected_child_ctx_ids = std::pair<ctx_id_t, ctx_id_t>(
            leftRank<0?0:_tree.leftChildContextId, 
            rightRank<0?0:_tree.rightChildContextId
        );
        _received_child_elems = std::pair<bool, bool>(false, false);

        _parent_rank = _tree.parentNodeRank;
        _parent_index = _tree.parentIndex;
        _parent_ctx_id = _tree.parentContextId;

        _is_root = _tree.index == 0;
        _base_msg.treeIndexOfSender = _tree.index;
        _base_msg.contextIdOfSender = _tree.contextId;

        int actual_num_children = 0;
        if (leftRank>=0) actual_num_children++;
        if (rightRank>=0) actual_num_children++;
        LOG(VERB_ALLRED, "SWEEP New RED. (%i)children [%i],[%i] \n", _tree.nbChildren, leftRank, rightRank);
        if (actual_num_children != _tree.nbChildren) {
            LOG(V1_WARN, "WARN SWEEP: AllReduction got Snapshot with _tree.nbChildren %i, but actual child ranks [%i]&[%i] give %i actual children! \n", _tree.nbChildren, leftRank, rightRank, actual_num_children);
        }
    }

    // Contribute to the all-reduction.
    void contribute(AllReduceElement&& localProducer) {
        assert(!_contributed);
        _contributed = true;
        _local_elem = std::move(localProducer);
    }

    void setTransformationOfElementAtRoot(std::function<AllReduceElement(const AllReduceElement&)> transformation) {
        _transformation_at_root = transformation;
        _has_transformation_at_root = true;
        if (_has_inplace_transformation_at_root) {
            LOG(V1_WARN, "WARN: You are setting a copying rootTransformation callback, but there is already an inplace version provided!\n");
        }
    }

    void setInplaceTransformationOfElementAtRoot(std::function<void(AllReduceElement&)> inplace_transformation) {
        _inplace_transformation_at_root = inplace_transformation;
        _has_inplace_transformation_at_root = true;
        if (_has_transformation_at_root) {
            LOG(V1_WARN, "WARN: You are setting an inplace rootTransformation callback, but there is already a copying version provided!\n");
        }
    }

    void enableBroadcast() {
        _broadcast_enabled = true;
    }
    void disableBroadcast() {
        _broadcast_enabled = false;
    }

    const JobTreeSnapshot& getJobTreeSnapshot() const {
        return _tree;
    }

private:
    // Process an incoming message and advance the all-reduction accordingly. 
    bool receive(int source, int tag, JobMessage& msg) {

        assert(tag == MSG_JOB_TREE_MODULAR_REDUCE || tag == MSG_JOB_TREE_MODULAR_BROADCAST || log_return_false("[WARN] Unexpected tag %i (msg.tag %i) in JobTreeAllReduction receive(...) from source %i\n", tag, msg.tag, source));

        LOG(V3_VERB, "TRY REDUCE %i %i %i %i %i\n", tag, msg.epoch, _base_msg.epoch, msg.tag, _base_msg.tag);

        bool accept = msg.epoch == _base_msg.epoch 
                    //&& msg.revision == _base_msg.revision 
                    && msg.tag == _base_msg.tag;
        if (!accept) return false;

        if (msg.returnedToSender && tag == MSG_JOB_TREE_MODULAR_REDUCE) {
            //A race-condition occured, the reduction element we sent upwards to our parent bounced back 
            _returnToSender_counter++;
            LOG(V1_WARN, "WARN: AllReduce got %i. returnedToSender msg (source %i, tag %i, msg.tag %i, msg.size %i)\n", _returnToSender_counter, source, tag, msg.tag, msg.payload.size());
            _returnToSender_payload = std::move(msg.payload);
            _have_unanswered_returnToSender = true;
            return true;
        }

        if (tag == MSG_JOB_TREE_MODULAR_REDUCE) {
            LOG(V2_INFO, "REDUCE\n");

            if (_aggregating || _future_aggregate.valid() || _reduction_locally_done) 
                return false; // already internally aggregating elements (or already done)!

            // check if this message comes from a child which didn't already send something
            bool fromLeftChild = !_received_child_elems.first && source == _expected_child_ranks.first;
            bool fromRightChild = !_received_child_elems.second && source == _expected_child_ranks.second;
            accept &= fromLeftChild || fromRightChild;
            if (!accept) return false;
            
            // message accepted: store and check off
            _child_elems.insert({source, std::move(msg.payload)});
            if (fromLeftChild) _received_child_elems.first = true;
            if (fromRightChild) _received_child_elems.second = true;
            LOG_ADD_SRC(V5_DEBG, "CS got %i/%i elems", source, _child_elems.size(), _num_expected_child_elems);
            advance();
        }
        if (tag == MSG_JOB_TREE_MODULAR_BROADCAST && _broadcast_enabled) {
            LOG(V2_INFO, "BROADCAST\n");
            receiveAndForwardFinalElem(std::move(msg.payload));
        }
        return true;
    }

public:
    // Advances the all-reduction, e.g., because the local producer finished
    // or the aggregation function finished. No-op if getResult() was already called.
    JobTreeAllReduction& advance() {

        if (_finished) return *this;

        LOG(VERB_ALLRED, "SWEEP [%i] RED g(%i/%i) lc(%i) ch[%i][%i] rcvd(%i,%i)\n",
            _tree.nodeRank,  _child_elems.size(), _num_expected_child_elems, _local_elem.has_value(), _expected_child_ranks.first, _expected_child_ranks.second,
            _received_child_elems.first, _received_child_elems.second);

        // In the Broadcast+Allreduction setup a race condition can occur, when one child reacts 'too quickly' w.r.t the other child.
        // Specifically, the fast child returns messages for both the Broadcast and the AllReduce step, but the parent
        // is not yet ready for the incoming AllReduce message, because it is still waiting for the Broadcast response from the other child.
        // As a result, the AllReduce is either stuck eternally.
        // This occurred especially when the Bcast+AllRed period was aggressively lowered,
        // to ca. 2ms periods between advance() calls  (PeriodicEvent<2,1>) instead of the default 10ms timing
        // and more frequent initialisations of new Broadcast rounds in general (every 20-30ms)
        //
        // Following, a (too) detailed example of the race condition, with root node 0 and children 1 and 2
        //
        //        0
        //       / \
        //      1  2
        //
        // - root 0 wants to start a Broadcast+AllReduction
        // - root 0 initializes its broadcast object _bcast, but not yet its reduction object _red
        //      _red stays uninitialized, in part because it needs the yet-to-be-created tree snapshot from broadcast
        // - root 0 sends a broadcast to 1 and 2
        // - child 2 receives the broadcast.
        //     it sends an acknowledgment back to root that it received the broadcast (MSG_JOB_TREE_MODULAR_BROADCAST)
        //     it also immediately evaluates hasResult()==true, since it doesnt have any children on its own
        //     it triggers its own broadcast callback (digestBroadcast() in the case of collectives_example_job.hpp)
        //     it initializes its reduction object _red
        //     it contributes to its reduction object
        // - root 0 receives the broadcast acknowledgment from child 2 and accordingly toggles _received_response_right=true
        //     however, it still waits for the broadcast response from child 1
        //     so its own hasResult() stays false, meaning it doesn't trigger its callback and _red stays uninitialized
        // - child 2 calls the periodic tryEndReduction(), triggering _red->advance(), starting the local aggregation
        // - child 2 calls the periodic tryEndReduction() again, sending the reduction element upwards to the parent (MSG_JOB_TREE_MODULAR_REDUCE)
        // - root 0 receives the reduction message, but doesnt have any listening objects yet to handle it
        //     in particular, root 0 is missing the  _red object, since its still waiting for child 1's broadcast response
        // - child 2 receives the "returnedToSender" message
        // - we are stuck, because root 0 now waits indefinitely for child 2's reduction element, but child 2 never sends it again
        //
        // THE HOTFIX:
        // - child 2 tries to send the same message again after it bounced, hoping that at some point the parent can handle it

        if (_have_unanswered_returnToSender) {
            LOG(V1_WARN, "WARN RED : sending %i. fixing message to parent after returnedToSender \n", _returnToSender_counter);
            _base_msg.payload = std::move(_returnToSender_payload);
            _base_msg.treeIndexOfDestination = _parent_index;
            _base_msg.contextIdOfDestination = _parent_ctx_id;
            assert(_base_msg.contextIdOfDestination != 0);
            LOG(V3_VERB, "SWEEP [%i] RED ~~~~%i~~>> [%i] to parent (is returnedToSender attempt) \n",_tree.nodeRank, _base_msg.payload.size(), _parent_rank);
            MyMpi::isend(_parent_rank, MSG_JOB_TREE_MODULAR_REDUCE, _base_msg);
            //Now that we re-send, the problem is no longer pending -- unless we get the error again, which would repeat this cycle
            _have_unanswered_returnToSender = false;
            return *this;
        }

        if (_child_elems.size() == _num_expected_child_elems && _local_elem.has_value()) {
             
            _child_elems.insert({-1, std::move(_local_elem.value())});
            _local_elem.reset();

            assert(!_future_aggregate.valid());
            _aggregating = true;
            _future_aggregate = ProcessWideThreadPool::get().addTask([&]() {
                std::list<AllReduceElement> elemsList;
                for (auto& childElem : _child_elems) elemsList.push_back(std::move(childElem.elem));
                _aggregated_elem = _aggregator(elemsList);
                _aggregating = false;
            });
        }

        if (!_aggregating && _future_aggregate.valid()) {
            // Aggregation done
            LOG(V5_DEBG, "CS got aggregation\n");

            _future_aggregate.get();
            _reduction_locally_done = true;
            
            if (_is_root) {
                // Transform reduced element at root
                if (_has_transformation_at_root) {
                    _aggregated_elem.emplace(_transformation_at_root(_aggregated_elem.value()));
                }
                //A non-const transformation is used to avoid copying the whole vector, when possible
                if (_has_inplace_transformation_at_root) {
                   _inplace_transformation_at_root(_aggregated_elem.value());
                }
                if (_broadcast_enabled) {// receive final elem and begin broadcast
                    receiveAndForwardFinalElem(std::move(_aggregated_elem.value()));
                } else { // only receive final elem
                    receiveFinalElem(std::move(_aggregated_elem.value()));
                }
            } else {
                // Send to parent
                _base_msg.payload = std::move(_aggregated_elem.value());
                _base_msg.treeIndexOfDestination = _parent_index;
                _base_msg.contextIdOfDestination = _parent_ctx_id;
                assert(_base_msg.contextIdOfDestination != 0);
                MyMpi::isend(_parent_rank, MSG_JOB_TREE_MODULAR_REDUCE, _base_msg);
            }
        }

        return *this;
    }

    void cancel() {

        if (_finished) return;

        if (!_reduction_locally_done) {
            // Aggregation upwards was not performed yet: Send neutral element upwards
            _base_msg.payload = _neutral_elem;
            _base_msg.treeIndexOfDestination = _parent_index;
            _base_msg.contextIdOfDestination = _parent_ctx_id;
            assert(_base_msg.contextIdOfDestination != 0);
            MyMpi::isend(_parent_rank, MSG_JOB_TREE_MODULAR_REDUCE, _base_msg);
        }
        // finished but not valid
        _finished = true;
        _valid = false;
    }

    bool hasContribution() const {return _contributed;}
    bool isValid() const {return _valid;}

    // Whether the final result to the all-reduction is present.
    bool hasResult() const {return _finished && _valid;}

    // Extract the final result to the all-reduction. hasResult() must be true.
    // After this call, hasResult() returns false.
    AllReduceElement extractResult() {
        assert(hasResult());
        _valid = false;
        return std::move(_base_msg.payload);
    }

    // Whether this object can be destructed at this point in time 
    // without waiting for another thread.
    bool isDestructible() const {
        if (_future_aggregate.valid() && _aggregating) return false;
        return true;
    }

    void destroy() {
        if (_future_aggregate.valid()) _future_aggregate.get();
    }

    ~JobTreeAllReduction() {
        destroy();
    }

private:
    void receiveFinalElem(AllReduceElement&& elem) {
        _finished = true;
        _base_msg.payload = std::move(elem);
    }

    void receiveAndForwardFinalElem(AllReduceElement&& elem) {
        receiveFinalElem(std::move(elem));
        if (_expected_child_ranks.first >= 0) {
            _base_msg.treeIndexOfDestination = _expected_child_indices.first;
            _base_msg.contextIdOfDestination = _expected_child_ctx_ids.first;
            assert(_base_msg.contextIdOfDestination != 0);
            MyMpi::isend(_expected_child_ranks.first, MSG_JOB_TREE_MODULAR_BROADCAST, _base_msg);
        }
        if (_expected_child_ranks.second >= 0) {
            _base_msg.treeIndexOfDestination = _expected_child_indices.second;
            _base_msg.contextIdOfDestination = _expected_child_ctx_ids.second;
            assert(_base_msg.contextIdOfDestination != 0);
            MyMpi::isend(_expected_child_ranks.second, MSG_JOB_TREE_MODULAR_BROADCAST, _base_msg);
        }
    }
};
