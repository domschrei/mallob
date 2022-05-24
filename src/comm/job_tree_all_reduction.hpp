
#pragma once

#include <list>

#include "app/job_tree.hpp"
#include "util/sys/thread_pool.hpp"

class JobTreeAllReduction {

public:
    typedef std::vector<int> AllReduceElement;

private:
    JobTree& _tree;
    JobMessage _base_msg;
    AllReduceElement _neutral_elem;
    
    std::optional<AllReduceElement> _local_elem;
    std::list<AllReduceElement> _child_elems;
    int _num_expected_child_elems;

    bool _aggregating = false;
    std::future<void> _future_aggregate;
    std::function<AllReduceElement(std::list<AllReduceElement>&)> _aggregator;
    std::optional<AllReduceElement> _aggregated_elem;

    bool _has_transformation_at_root = false;
    std::function<AllReduceElement(const AllReduceElement&)> _transformation_at_root;

    bool _has_producer = false;
    bool _reduction_locally_done = false;
    bool _finished = false;
    bool _valid = true;

public:
    JobTreeAllReduction(JobTree& jobTree, JobMessage baseMsg, AllReduceElement&& neutralElem, 
            std::function<AllReduceElement(std::list<AllReduceElement>&)> aggregator) :
        _tree(jobTree), _base_msg(baseMsg), _neutral_elem(std::move(neutralElem)), 
        _num_expected_child_elems(_tree.getNumChildren()), _aggregator(aggregator) {}

    // Set the function to compute the local contribution for the all-reduction.
    // This function is invoked immediately
    void produce(std::function<AllReduceElement()> localProducer) {
        assert(!_has_producer);
        _has_producer = true;
        _local_elem = localProducer();
    }

    void setTransformationOfElementAtRoot(std::function<AllReduceElement(const AllReduceElement&)> transformation) {
        _transformation_at_root = transformation;
        _has_transformation_at_root = true;
    }

    // Process an incoming message and advance the all-reduction accordingly. 
    // The element is accepted only if the provided acceptor function admits it.
    bool receive(int source, int tag, JobMessage& msg) {

        assert(tag == MSG_JOB_TREE_REDUCTION || tag == MSG_JOB_TREE_BROADCAST);

        bool accept = msg.jobId == _base_msg.jobId 
                    && msg.epoch == _base_msg.epoch 
                    && msg.revision == _base_msg.revision 
                    && msg.tag == _base_msg.tag;
        if (!accept) return false;

        if (tag == MSG_JOB_TREE_REDUCTION) {
            if (!_aggregating) {
                _child_elems.push_back(std::move(msg.payload));
                LOG_ADD_SRC(V5_DEBG, "CS got %i/%i elems", source, _child_elems.size(), _num_expected_child_elems);
                advance();
            }
        }
        if (tag == MSG_JOB_TREE_BROADCAST) {
            receiveAndForwardFinalElem(std::move(msg.payload));
        }
        return true;
    }

    // Advances the all-reduction, e.g., because the local producer finished
    // or the aggregation function finished. No-op if getResult() was already called.
    void advance() {

        if (_finished) return;

        if (_child_elems.size() == _num_expected_child_elems && _local_elem.has_value()) {
             
            _child_elems.push_front(std::move(_local_elem.value()));
            _local_elem.reset();

            assert(!_future_aggregate.valid());
            _aggregating = true;
            _future_aggregate = ProcessWideThreadPool::get().addTask([&]() {
                _aggregated_elem = _aggregator(_child_elems);
                _aggregating = false;
            });
        }

        if (!_aggregating && _future_aggregate.valid()) {
            // Aggregation done
            _future_aggregate.get();
            _reduction_locally_done = true;
            
            if (_tree.isRoot()) {
                // Transform reduced element at root
                if (_has_transformation_at_root) {
                    _aggregated_elem.emplace(_transformation_at_root(_aggregated_elem.value()));
                }
                // Begin broadcast
                receiveAndForwardFinalElem(std::move(_aggregated_elem.value()));
            } else {
                // Send to parent
                _base_msg.payload = std::move(_aggregated_elem.value());
                MyMpi::isend(_tree.getParentNodeRank(), MSG_JOB_TREE_REDUCTION, _base_msg);
            }
        }
    }

    void cancel() {

        if (_finished) return;

        if (!_reduction_locally_done) {
            // Aggregation upwards was not performed yet: Send neutral element upwards
            _base_msg.payload = _neutral_elem;
            MyMpi::isend(_tree.getParentNodeRank(), MSG_JOB_TREE_REDUCTION, _base_msg);
        }
        // finished but not valid
        _finished = true;
        _valid = false;
    }

    bool hasProducer() const {return _has_producer;}
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
    void receiveAndForwardFinalElem(AllReduceElement&& elem) {
        _finished = true;
        _base_msg.payload = std::move(elem);
        if (_tree.hasLeftChild()) 
            MyMpi::isend(_tree.getLeftChildNodeRank(), MSG_JOB_TREE_BROADCAST, _base_msg);
        if (_tree.hasRightChild()) 
            MyMpi::isend(_tree.getRightChildNodeRank(), MSG_JOB_TREE_BROADCAST, _base_msg);
    }

};
