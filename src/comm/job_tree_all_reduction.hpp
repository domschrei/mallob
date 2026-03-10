
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
    bool _aggregated_logging = false; //just for logging purposes
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

    bool _parent_is_ready = true;
    bool _care_about_parent_status = false;

    CondMessageSubscription _sub_aggregate;
    CondMessageSubscription _sub_broadcast;
    CondMessageSubscription _sub_parent_status;

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
            LOG(V3_VERB, "BROADCAST\n");
            JobMessage msg = Serializable::get<JobMessage>(h.getRecvData());
            return receive(h.source, h.tag, msg);
        }),
        _sub_parent_status(MSG_JOB_TREE_PARENT_IS_READY, [this](MessageHandle& h) {
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

        int act_num_children = 0;
        if (leftRank>=0) act_num_children++;
        if (rightRank>=0) act_num_children++;
        LOG(V3_VERB, "SWEEP New _red AllReduction. Snapshot: LeftChild [%i], RightChild [%i], _tree.nbChildren %i.\n", leftRank, rightRank, _tree.nbChildren);
        if (act_num_children != _tree.nbChildren) {
            LOG(V1_WARN, "WARN SWEEP: AllReduction got Snapshot with _tree.nbChildren %i, but actual child ranks [%i]&[%i] give %i actual children! \n", _tree.nbChildren, leftRank, rightRank, act_num_children);
        }
    }

    // Contribute to the all-reduction.
    void contribute(AllReduceElement&& localProducer) {
        assert(!_contributed);
        _contributed = true;
        // LOG(V3_VERB, "   contribute \n");
        _local_elem = std::move(localProducer);
    }

    void setTransformationOfElementAtRoot(std::function<AllReduceElement(const AllReduceElement&)> transformation) {
        _transformation_at_root = transformation;
        _has_transformation_at_root = true;
        if (_has_inplace_transformation_at_root) {
            LOG(V1_WARN, "WARN: Setting copying rootTransformation when there already exists its inplace pendant!\n");
        }
    }

    void setInplaceTransformationOfElementAtRoot(std::function<void(AllReduceElement&)> inplace_transformation) {
        _inplace_transformation_at_root = inplace_transformation;
        _has_inplace_transformation_at_root = true;
        if (_has_transformation_at_root) {
            LOG(V1_WARN, "WARN: Setting inplace rootTransformation when there already exists its coyping pendant!\n");
        }
    }

    void enableBroadcast() {
        _broadcast_enabled = true;
    }
    void disableBroadcast() {
        _broadcast_enabled = false;
    }

    void setCareAboutParent() {
        _care_about_parent_status = true;
        if (!_is_root) {
            _parent_is_ready = false;
        }
        // tellChildrenParentIsReady();
    }

    void enableParentIsReady() {
        _parent_is_ready = true;
    }

    const JobTreeSnapshot& getJobTreeSnapshot() const {
        return _tree;
    }

private:
    // Process an incoming message and advance the all-reduction accordingly.
    bool receive(int source, int tag, JobMessage& msg) {

        assert(tag == MSG_JOB_TREE_MODULAR_REDUCE || tag == MSG_JOB_TREE_MODULAR_BROADCAST || tag == MSG_JOB_TREE_PARENT_IS_READY || log_return_false("SWEEP Warn: Unexpected tag %i (msg.tag %i) in JobTreeAllReduction receive(...) from source %i\n", tag, msg.tag, source));

        if (!_care_about_parent_status)
            LOG(V5_DEBG, "TRY REDUCE %i %i %i %i %i\n", tag, msg.epoch, _base_msg.epoch, msg.tag, _base_msg.tag);

        bool accept = msg.epoch == _base_msg.epoch
                    //&& msg.revision == _base_msg.revision
                    && msg.tag == _base_msg.tag;
        if (!accept) return false;

        if (msg.returnedToSender) {
            _returnToSender_counter++;
            LOG(V1_WARN, "WARN RED REDUCE : got %i-th returnedToSender (source %i, tag %i, msg.tag %i, msg.size %i)\n", _returnToSender_counter, source, tag, msg.tag, msg.payload.size());
            _returnToSender_payload = std::move(msg.payload);
            _have_unanswered_returnToSender = true;
            return true;
        }

        if (tag == MSG_JOB_TREE_MODULAR_REDUCE) {
            LOG(V3_VERB, "REDUCE\n");

            if (_aggregating || _future_aggregate.valid() || _reduction_locally_done)
                return false; // already internally aggregating elements (or already done)!

            // check if this message comes from a child which didn't already send something
            bool fromLeftChild = !_received_child_elems.first && source == _expected_child_ranks.first;
            bool fromRightChild = !_received_child_elems.second && source == _expected_child_ranks.second;
            accept &= fromLeftChild || fromRightChild;
            if (!accept) return false;

            LOG(V4_VVER, "SWEEP RED received elem from child [%i], size %i \n", source, msg.payload.size());
            // message accepted: store and check off
            _child_elems.insert({source, std::move(msg.payload)});
            if (fromLeftChild) _received_child_elems.first = true;
            if (fromRightChild) _received_child_elems.second = true;
            LOG_ADD_SRC(V5_DEBG, "CS got %i/%i elems", source, _child_elems.size(), _num_expected_child_elems);
            advance();
        }
        if (tag == MSG_JOB_TREE_MODULAR_BROADCAST && _broadcast_enabled) {
            LOG(V3_VERB, "BROADCAST\n");
            receiveAndForwardFinalElem(std::move(msg.payload));
        }
        if (tag == MSG_JOB_TREE_PARENT_IS_READY) {
            LOG(V3_VERB, "  learned that parent %i is ready\n", source);
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

        LOG(V4_VVER, "SWEEP RED advance() exp.childs=%i, elems=%i, local=%i. childranks [%i],[%i]. (%i)recvd_left, (%i)recvd_right,  (%i)aggregating, (%i)aggregated, (%i)future_valid \n",
            _num_expected_child_elems, _child_elems.size(), _local_elem.has_value(), _expected_child_ranks.first, _expected_child_ranks.second,
            _received_child_elems.first, _received_child_elems.second, _aggregating, _aggregated_logging, _future_aggregate.valid());



        //It can happen that a reduction sent to the parent gets returned via the returnToSender error.
        //Afaik, this can happen (in rare cases) with combined modular BCAST+ALLRED, the following way
        // 1.A Parent sends a BCAST out
        // 2.One of its leaf nodes reacts extremely quickly to this BCAST. It sends its BCAST information back and immediately creates an ALLRED object and has it contribute to the parent ALLRED
        // 3.The parent node, more or less concurrently, now also receives all the BCAST information and creates an ALLRED object, filled with the correct snapshot
        // 4.Since the parent node might have been a bit slower, the ALLRED message from the leaf node had not corresponding ALLRED object to arrive and, and gets returnedToSender

        //We resolve this problem by remembering a returnToSender error at the child, and retrying to sending it again
        if (_have_unanswered_returnToSender) {
            LOG(V1_WARN, "WARN RED : sending %i-th fixing message to parent after returnedToSender \n", _returnToSender_counter);
            _base_msg.payload = std::move(_returnToSender_payload);
            _base_msg.treeIndexOfDestination = _parent_index;
            _base_msg.contextIdOfDestination = _parent_ctx_id;
            assert(_base_msg.contextIdOfDestination != 0);
            LOG(V3_VERB, "SWEEP RED [%i] advance ~~~%i~~~> to parent [%i]\n",_tree.nodeRank, _base_msg.payload.size(), _parent_rank);
            MyMpi::isend(_parent_rank, MSG_JOB_TREE_MODULAR_REDUCE, _base_msg);
            //Now that we re-send, the problem is no longer pending -- unless we get the error again, which would repeat this cycle
            _have_unanswered_returnToSender = false;
            return *this;
        }


        //at child_elems == expected_elems we briefly add our own element to the children, so then we have one more child element than expected, but thats no longer an issue to the propagate the combined aggregated element
        if (_child_elems.size() == _num_expected_child_elems && _local_elem.has_value()) {

            // LOG(V4_VVER, "SWEEP RED queued agg\n");
            // LOG(V4_VVER, "SWEEP SHARE AGGR queuing aggregation thread\n");
            _child_elems.insert({-1, std::move(_local_elem.value())});
            _local_elem.reset();

            assert(!_future_aggregate.valid());
            _aggregating = true;
            _future_aggregate = ProcessWideThreadPool::get().addTask([&]() {
                // LOG(V4_VVER, "SWEEP SHARE AGGR started own aggregation thread\n");
                // LOG(V4_VVER, "SWEEP RED started agg\n");
                std::list<AllReduceElement> elemsList;
                for (auto& childElem : _child_elems) elemsList.push_back(std::move(childElem.elem));
                _aggregated_elem = _aggregator(elemsList);
                _aggregating = false;
                _aggregated_logging = true;
                // LOG(V4_VVER, "SWEEP RED done agg\n");
            });
        }

        // LOG(V4_VVER, "SWEEP SHARE ADV: aggregating %i, future_aggregate.valid() %i, reduction_locally_done %i (already send aggregated element) \n", _aggregating, _future_aggregate.valid(), _reduction_locally_done);

        if (!_aggregating && _future_aggregate.valid() && _parent_is_ready) {
            // Aggregation done
            // LOG(V5_DEBG, "CS got aggregation\n");
            // LOG(V4_VVER, "SWEEP RED advancing to parent or broadcasting \n");

            _future_aggregate.get();
            _reduction_locally_done = true;

            if (_is_root) {
                // Transform reduced element at root
                if (_has_transformation_at_root) {
                    _aggregated_elem.emplace(_transformation_at_root(_aggregated_elem.value()));
                }

                if (_has_inplace_transformation_at_root) {
                   _inplace_transformation_at_root(_aggregated_elem.value());
                }

                if (_broadcast_enabled) {// receive final elem and begin broadcast
                    LOG(V3_VERB, "SWEEP RED broadcasting result \n");
                    receiveAndForwardFinalElem(std::move(_aggregated_elem.value()));
                } else { // only receive final elem
                    receiveFinalElem(std::move(_aggregated_elem.value()));
                }
            } else {
                // Send to parent
                _base_msg.payload = std::move(_aggregated_elem.value());
                _base_msg.treeIndexOfDestination = _parent_index;
                _base_msg.contextIdOfDestination = _parent_ctx_id;
                LOG(V3_VERB, "SWEEP RED [%i] advance ~~~%i~~~> to parent [%i]\n",_tree.nodeRank, _base_msg.payload.size(), _parent_rank);
                assert(_base_msg.contextIdOfDestination != 0);
                MyMpi::isend(_parent_rank, MSG_JOB_TREE_MODULAR_REDUCE, _base_msg);
                if (_care_about_parent_status) {
                    _parent_is_ready = false;
                }
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

    void tellChildrenParentIsReady() {
        if (_expected_child_ranks.first >= 0) {
            _base_msg.treeIndexOfDestination = _expected_child_indices.first;
            _base_msg.contextIdOfDestination = _expected_child_ctx_ids.first;
            LOG(V3_VERB, "      tell child %i I'm ready\n", _expected_child_indices.first);
            assert(_base_msg.contextIdOfDestination != 0);
            MyMpi::isend(_expected_child_ranks.first, MSG_JOB_TREE_PARENT_IS_READY, _base_msg);

        }
        if (_expected_child_ranks.second >= 0) {
            _base_msg.treeIndexOfDestination = _expected_child_indices.second;
            _base_msg.contextIdOfDestination = _expected_child_ctx_ids.second;
            LOG(V3_VERB, "      tell child %i I'm ready \n", _expected_child_indices.second);
            assert(_base_msg.contextIdOfDestination != 0);
            MyMpi::isend(_expected_child_ranks.second, MSG_JOB_TREE_PARENT_IS_READY, _base_msg);
        }
    }


    bool hasContribution() const {return _contributed;}
    bool isValid() const {return _valid;}
    bool isParentReady() const {return _parent_is_ready;}

    bool finishedAndNoLongerValid() const {return _finished && !_valid;}

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
        // LOG(V3_VERB, "      -- destroy JobTree --\n");
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
        LOG(V3_VERB, "SWEEP SHARE receive and forwarding final element \n");
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
