
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
    std::future<void> _future_aggregate;
    std::function<AllReduceElement(std::list<AllReduceElement>&)> _aggregator;
    std::optional<AllReduceElement> _aggregated_elem;

    bool _has_transformation_at_root = false;
    std::function<AllReduceElement(const AllReduceElement&)> _transformation_at_root;

    bool _contributed = false;
    bool _reduction_locally_done = false;
    bool _finished = false;
    bool _valid = true;
    bool _broadcast_enabled = true;
    bool _parent_is_ready = false;

    CondMessageSubscription _sub_aggregate;
    CondMessageSubscription _sub_broadcast;
    CondMessageSubscription _sub_parent_update;

public:
    JobTreeAllReduction(const JobTreeSnapshot& tree, JobMessage baseMsg, AllReduceElement&& neutralElem, 
            std::function<AllReduceElement(std::list<AllReduceElement>&)> aggregator) :
        _tree(tree), _base_msg(baseMsg), _neutral_elem(std::move(neutralElem)),
        _num_expected_child_elems(_tree.nbChildren), _aggregator(aggregator),
        _sub_aggregate(MSG_JOB_TREE_MODULAR_REDUCE, [this](MessageHandle& h) {
            JobMessage msg = Serializable::get<JobMessage>(h.getRecvData());
            return receive(h.source, h.tag, msg);
        }),
        _sub_broadcast(MSG_JOB_TREE_MODULAR_BROADCAST, [this](MessageHandle& h) {
            LOG(V2_INFO, "BROADCAST\n");
            JobMessage msg = Serializable::get<JobMessage>(h.getRecvData());
            return receive(h.source, h.tag, msg);
        }),
        _sub_parent_update(MSG_JOB_TREE_PARENT_IS_READY, [this](MessageHandle& h) {
            JobMessage msg = Serializable::get<JobMessage>(h.getRecvData());
            return receive(h.source, h.tag, msg);
        })
    {

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
        if (_is_root) {
            _parent_is_ready = true; //root doesnt have any parents to wait for
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
    }

    void enableBroadcast() {
        _broadcast_enabled = true;
    }
    void disableBroadcast() {
        _broadcast_enabled = false;
    }

    void enableParentIsReady() {
        _parent_is_ready = true;
    }

    bool isParentReady() const {
        return _parent_is_ready;
    }


    const JobTreeSnapshot& getJobTreeSnapshot() const {
        return _tree;
    }

private:
    // Process an incoming message and advance the all-reduction accordingly. 
    bool receive(int source, int tag, JobMessage& msg) {

        LOG(V3_VERB, "      received tag %i from %i\n", tag, source);

        // Forgot to add PARENT_IS_READY to this assertion! ß
        assert(tag == MSG_JOB_TREE_MODULAR_REDUCE || tag == MSG_JOB_TREE_MODULAR_BROADCAST || tag == MSG_JOB_TREE_PARENT_IS_READY);
        // assert(tag == MSG_JOB_TREE_MODULAR_REDUCE || tag == MSG_JOB_TREE_MODULAR_BROADCAST);

        LOG(V2_INFO, "TRY REDUCE %i %i %i %i %i\n", tag, msg.epoch, _base_msg.epoch, msg.tag, _base_msg.tag);

        bool accept = msg.epoch == _base_msg.epoch 
                    //&& msg.revision == _base_msg.revision 
                    && msg.tag == _base_msg.tag;
        if (!accept) return false;

        if (msg.returnedToSender) {
            LOG(V2_INFO, "REDUCE returnedToSender\n");
            msg.swapSenderReceiver();
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
            LOG(V3_VERB, "      Received from child %i \n", source);
            advance();
        }
        if (tag == MSG_JOB_TREE_MODULAR_BROADCAST && _broadcast_enabled) {
            LOG(V2_INFO, "BROADCAST\n");
            receiveAndForwardFinalElem(std::move(msg.payload));
        }
        if (tag == MSG_JOB_TREE_PARENT_IS_READY) {
            LOG(V3_VERB, "      Learnt that parent is ready\n");
            _parent_is_ready = true;
            advance();
        }
        return true;
    }

public:
    // Advances the all-reduction, e.g., because the local producer finished
    // or the aggregation function finished. No-op if getResult() was already called.
    JobTreeAllReduction& advance() {

        if (_finished) return *this;

        // LOG(V3_VERB, "local elem exist: %i \n ",  _local_elem.has_value());
        // LOG(V3_VERB, "child elem count: %i \n ", _child_elems.size());
        // LOG(V3_VERB, "child elem expec: %i \n ", _num_expected_child_elems);


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

        LOG(V3_VERB, "              adv status: _aggregating    = %i \n ", _aggregating);
        LOG(V3_VERB, "              adv status: _future.valid() = %i \n ", _future_aggregate.valid());
        LOG(V3_VERB, "              adv status: _child_elems    = %i \n ", _child_elems.size());
        LOG(V3_VERB, "              adv status: _parent_is_ready= %i \n ", _parent_is_ready);

        if (!_aggregating && _future_aggregate.valid() && _parent_is_ready) {
            // Aggregation done
            LOG(V5_DEBG, "CS got aggregation\n");

            _future_aggregate.get();
            _reduction_locally_done = true;
            
            if (_is_root) {
                // Transform reduced element at root
                if (_has_transformation_at_root) {
                    _aggregated_elem.emplace(_transformation_at_root(_aggregated_elem.value()));
                }

                if (_broadcast_enabled) {// receive final elem and begin broadcast

                    LOG(V3_VERB, "      root got all elements. broadcast \n ");
                    receiveAndForwardFinalElem(std::move(_aggregated_elem.value()));
                } else { // only receive final elem
                    receiveFinalElem(std::move(_aggregated_elem.value()));
                }
            } else {
                // Send to parent
                _base_msg.payload = std::move(_aggregated_elem.value());
                _base_msg.treeIndexOfDestination = _parent_index;
                _base_msg.contextIdOfDestination = _parent_ctx_id;
                LOG(V3_VERB, "      sending to parent %i \n ", _parent_index);
                MyMpi::isend(_parent_rank, MSG_JOB_TREE_MODULAR_REDUCE, _base_msg);
                _parent_is_ready = false;
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
        LOG(V3_VERB, "Extracting result\n");
        auto result = std::move(_base_msg.payload); //moving the payload now out of the base_msg
                                                    //such that the base_msg is just a lightweight dummy when now messaging the children
        _base_msg.payload = {}; //set dummy array to be sure that it is at least initialized
        LOG(V3_VERB, "tell children I am ready\n");
        tell_children_parent_is_ready();
        return result;
    }

    // Whether this object can be destructed at this point in time 
    // without waiting for another thread.
    bool isDestructible() const {
        if (_future_aggregate.valid() && _aggregating) return false;
        return true;
    }

    void destroy() {
        LOG(V3_VERB, " ## Destroy ## tag %i \n", _base_msg.tag);
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
            MyMpi::isend(_expected_child_ranks.first, MSG_JOB_TREE_MODULAR_BROADCAST, _base_msg);
        }
        if (_expected_child_ranks.second >= 0) {
            _base_msg.treeIndexOfDestination = _expected_child_indices.second;
            _base_msg.contextIdOfDestination = _expected_child_ctx_ids.second;
            MyMpi::isend(_expected_child_ranks.second, MSG_JOB_TREE_MODULAR_BROADCAST, _base_msg);
        }
    }

    void tell_children_parent_is_ready() {
        if (_expected_child_ranks.first >= 0) {
            _base_msg.treeIndexOfDestination = _expected_child_indices.first;
            _base_msg.contextIdOfDestination = _expected_child_ctx_ids.first;
            LOG(V3_VERB, " tell child %i \n", _expected_child_indices.first);
            MyMpi::isend(_expected_child_ranks.first, MSG_JOB_TREE_PARENT_IS_READY, _base_msg);
        }
        if (_expected_child_ranks.second >= 0) {
            _base_msg.treeIndexOfDestination = _expected_child_indices.second;
            _base_msg.contextIdOfDestination = _expected_child_ctx_ids.second;
            LOG(V3_VERB, " tell child %i \n", _expected_child_indices.second);
            MyMpi::isend(_expected_child_ranks.second, MSG_JOB_TREE_PARENT_IS_READY, _base_msg);
        }
    }
};
