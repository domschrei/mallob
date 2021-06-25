
#ifndef DOMPASCH_MALLOB_JOB_TREE_HPP
#define DOMPASCH_MALLOB_JOB_TREE_HPP

#include <set>
#include <assert.h>

#include "util/robin_hood.hpp"
#include "util/permutation.hpp"
#include "data/job_transfer.hpp"
#include "util/sys/timer.hpp"
#include "util/logger.hpp"

class JobTree {

private:
    const int _comm_size;
    const int _rank;
    int _index = -1;
    AdjustablePermutation _job_node_ranks;
    bool _has_left_child = false;
    bool _has_right_child = false;
    int _client_rank;
    robin_hood::unordered_set<int> _past_children;
    robin_hood::unordered_map<int, int> _dormant_children_num_fails;
    int _balancing_epoch_of_last_requests = -1;

public:
    JobTree(int commSize, int rank, int seed) : _comm_size(commSize), _rank(rank), _job_node_ranks(commSize, seed) {}

    int getIndex() const {return _index;}
    int getCommSize() const {return _comm_size;}
    int getRank() const {return _rank;}
    bool isRoot() const {return _index == 0;};
    int getRootNodeRank() const {return _job_node_ranks[0];};
    int getLeftChildNodeRank() const {return _job_node_ranks[getLeftChildIndex()];};
    int getRightChildNodeRank() const {return _job_node_ranks[getRightChildIndex()];};
    bool isLeaf() const {return !_has_left_child && !_has_right_child;}
    bool hasLeftChild() const {return _has_left_child;};
    bool hasRightChild() const {return _has_right_child;};
    int getLeftChildIndex() const {return 2*(_index+1)-1;};
    int getRightChildIndex() const {return 2*(_index+1);};
    int getParentNodeRank() const {return isRoot() ? _client_rank : _job_node_ranks[getParentIndex()];};
    int getParentIndex() const {return (_index-1)/2;};
    robin_hood::unordered_set<int>& getPastChildren() {return _past_children;}
    int getNumFailsOfDormantChild(int rank) const {
        assert(_dormant_children_num_fails.count(rank));
        return _dormant_children_num_fails.at(rank);
    }
    std::set<int> getDormantChildren() const {
        std::set<int> c;
        for (const auto& [child, _] : _dormant_children_num_fails) {
            c.insert(child);
        }
        return c;
    }
    void setBalancingEpochOfLastRequests(int epoch) {
        _balancing_epoch_of_last_requests = epoch;
    }
    int getBalancingEpochOfLastRequests() {
        return _balancing_epoch_of_last_requests;
    }

    enum TreeRelative {LEFT_CHILD, RIGHT_CHILD, NONE};
    JobRequest getJobRequestFor(int jobId, TreeRelative rel, int balancingEpoch) {
        return JobRequest(jobId, getRootNodeRank(), _rank, 
                rel == LEFT_CHILD ? getLeftChildIndex() : getRightChildIndex(), 
                Timer::elapsedSeconds(), balancingEpoch, 0);
    }
    TreeRelative prune(int leavingRank, int leavingIndex) {
        if (hasLeftChild() && getLeftChildIndex() == leavingIndex && getLeftChildNodeRank() == leavingRank) {
            unsetLeftChild();
            return LEFT_CHILD;
        } 
        if (hasRightChild() && getRightChildIndex() == leavingIndex && getRightChildNodeRank() == leavingRank) {
            unsetRightChild();
            return RIGHT_CHILD;
        }
        return NONE;
    }

    TreeRelative setChild(int rank, int index) {
        if (index == getLeftChildIndex()) {
            setLeftChild(rank);
            return LEFT_CHILD;
        }
        if (index == getRightChildIndex()) {
            setRightChild(rank);
            return RIGHT_CHILD;
        }
        return NONE;
    }
    void setLeftChild(int rank) {
        _has_left_child = true;
        updateJobNode(getLeftChildIndex(), rank);
    }
    void setRightChild(int rank) {
        _has_right_child = true;
        updateJobNode(getRightChildIndex(), rank);
    }
    void unsetLeftChild() {
        if (!_has_left_child) return; 
        int rank = getLeftChildNodeRank();
        _past_children.insert(rank);
        addDormantChild(rank);
        _has_left_child = false;
    }
    void unsetRightChild() {
        if (!_has_right_child) return;
        int rank = getRightChildNodeRank();
        _past_children.insert(rank); 
        addDormantChild(rank);
        _has_right_child = false;
    }
    void update(int index, int rootRank, int parentRank) {    
        _index = index;
        if (index == 0 || rootRank < 0) rootRank = _rank; // this is the root node
        updateJobNode(0, rootRank);
        updateJobNode(_index, _rank);
        updateParentNodeRank(parentRank);
    }
    void updateJobNode(int index, int newRank) {
        _job_node_ranks.adjust(index, newRank);
    }
    void clearJobNodeUpdates() {
        _job_node_ranks.clear();
    }
    void updateParentNodeRank(int newRank) {
        if (isRoot()) {
            // Root worker node!
            _client_rank = newRank;
        } else {
            // Inner node / leaf worker
            updateJobNode(getParentIndex(), newRank);
        }
    }
    int findDormantChild(int excludedRank) {
        for (const auto& [child, numFails] : _dormant_children_num_fails) {
            if (child != excludedRank) return child;
        }
        return -1;
    }
    void addDormantChild(int rank) {
        _dormant_children_num_fails[rank] = 0;
    }
    void addFailToDormantChild(int rank) {
        if (!_dormant_children_num_fails.count(rank)) return;
        _dormant_children_num_fails[rank]++;
        if (_dormant_children_num_fails[rank] >= 3)
            _dormant_children_num_fails.erase(rank);
    }
    void eraseDormantChild(int rank) {
        _dormant_children_num_fails.erase(rank);
    }

    bool isTransitiveParentOf(int index) {
        if (index == _index) return true;
        int lower = _index, upper = _index;
        while (lower < _comm_size) {
            lower = getLeftChildIndex(lower);
            upper = getRightChildIndex(upper);
            if (lower <= index && index <= upper) return true;
            if (index < lower) return false;
        }
        return false;
    }

private:
    static int getLeftChildIndex(int index) {return 2*(index+1)-1;}
    static int getRightChildIndex(int index) {return 2*(index+1);}
    static int getParentIndex(int index) {return (index-1)/2;}    

};

#endif