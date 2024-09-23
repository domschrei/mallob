
#ifndef DOMPASCH_MALLOB_JOB_TREE_HPP
#define DOMPASCH_MALLOB_JOB_TREE_HPP

#include <set>
#include <list>
#include "comm/job_tree_snapshot.hpp"
#include "comm/msg_queue/message_queue.hpp"
#include "comm/msgtags.h"
#include "util/assert.hpp"

#include "util/hashing.hpp"
#include "util/permutation.hpp"
#include "data/job_transfer.hpp"
#include "util/sys/timer.hpp"
#include "util/logger.hpp"
#include "comm/mympi.hpp"

class JobTree {

private:
    const int _comm_size;
    const int _rank;
    const int _ctx_id;
    int _index = -1;
    AdjustablePermutation _job_node_ranks;
    ctx_id_t _root_ctx_id {0};
    ctx_id_t _parent_ctx_id {0};
    ctx_id_t _left_child_ctx_id {0};
    ctx_id_t _right_child_ctx_id {0};
    int _client_rank;
    robin_hood::unordered_set<int> _past_children;

    bool _use_dormant_children;
    struct DormantChild {
        int rank;
        int numUses = 0;
        bool operator<(const DormantChild& other) const {
            return numUses < other.numUses;
        }
        bool operator==(const DormantChild& other) const {
            if (rank != other.rank) return false;
            return true;
        }
        bool operator!=(const DormantChild& other) const {
            return !(*this == other);
        }
    };
    std::list<int> _dormant_children;
    std::list<int>::iterator _it_dormant_children;
    
    int _balancing_epoch_of_last_requests = -1;

    float _time_of_desire_left = -1;
    float _time_of_desire_right = -1;
    size_t _num_desires = 0;
    size_t _num_fulfilled_desires = 0;
    float _sum_desire_latencies = 0;
    std::vector<float> _desire_latencies;

    std::list<int> _send_handles_left;
    std::list<int> _send_handles_right;

    int _wait_epoch = -1;
    int _stop_wait_epoch = -1;

public:
    JobTree(int commSize, int rank, ctx_id_t contextId, int seed, bool useDormantChildren) : 
        _comm_size(commSize), _rank(rank), _ctx_id(contextId), 
        _job_node_ranks(commSize, seed), 
        _use_dormant_children(useDormantChildren) {
        
        if (_use_dormant_children) _it_dormant_children = _dormant_children.begin();
    }

    int getIndex() const {return _index;}
    int getCommSize() const {return _comm_size;}
    int getRank() const {return _rank;}
    bool isRoot() const {return _index == 0;}
    int getRootNodeRank() const {return _job_node_ranks[0];}
    int getLeftChildNodeRank() const {
        int index = getLeftChildIndex();
        return index < _comm_size ? _job_node_ranks[index] : -1;
    }
    int getRightChildNodeRank() const {
        int index = getRightChildIndex();
        return index < _comm_size ? _job_node_ranks[index] : -1;
    }
    bool isLeaf() const {return !hasLeftChild() && !hasRightChild();}
    int getLeftChildIndex() const {return 2*(_index+1)-1;}
    int getRightChildIndex() const {return 2*(_index+1);}
    int getParentNodeRank() const {return isRoot() ? _client_rank : _job_node_ranks[getParentIndex()];}
    int getParentIndex() const {return (_index-1)/2;}
    robin_hood::unordered_set<int>& getPastChildren() {return _past_children;}
    int getRankOfNextDormantChild() {
        if (_dormant_children.empty()) return -1;
        int rank = *_it_dormant_children;
        _it_dormant_children++;
        if (_it_dormant_children == _dormant_children.end())
            _it_dormant_children = _dormant_children.begin();
        return rank;
    }
    void setBalancingEpochOfLastRequests(int epoch) {
        _balancing_epoch_of_last_requests = epoch;
    }
    int getBalancingEpochOfLastRequests() {
        return _balancing_epoch_of_last_requests;
    }

    enum TreeRelative {LEFT_CHILD, RIGHT_CHILD, NONE};
    JobRequest getJobRequestFor(int jobId, TreeRelative rel, int balancingEpoch, int appId, bool incremental) {
        JobRequest req(jobId, appId, getRootNodeRank(), _rank, 
                rel == LEFT_CHILD ? getLeftChildIndex() : getRightChildIndex(), 
                Timer::elapsedSeconds(), balancingEpoch, 0, incremental);
        req.rootContextId = getRootContextId();
        req.requestingNodeContextId = getContextId();
        return req;
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

    TreeRelative setChild(int rank, int index, ctx_id_t contextId) {
        assert(rank >= 0);
        if (index == getLeftChildIndex()) {
            setLeftChild(rank, contextId);
            return LEFT_CHILD;
        }
        if (index == getRightChildIndex()) {
            setRightChild(rank, contextId);
            return RIGHT_CHILD;
        }
        return NONE;
    }
    void setLeftChild(int rank, int contextId) {
        _left_child_ctx_id = contextId;
        updateJobNode(getLeftChildIndex(), rank);
        fulfilDesireLeft(Timer::elapsedSeconds());
    }
    void setRightChild(int rank, int contextId) {
        _right_child_ctx_id = contextId;
        updateJobNode(getRightChildIndex(), rank);
        fulfilDesireRight(Timer::elapsedSeconds());
    }

    bool hasLeftChild() const {
        return _left_child_ctx_id != 0;
    }
    bool hasRightChild() const {
        return _right_child_ctx_id != 0;
    }

    ctx_id_t getContextId() const {
        return _ctx_id;
    }
    ctx_id_t getRootContextId() const {
        return _root_ctx_id;
    }
    ctx_id_t getParentContextId() const {
        return _parent_ctx_id;
    }
    ctx_id_t getLeftChildContextId() const {
        return _left_child_ctx_id;
    }
    ctx_id_t getRightChildContextId() const {
        return _right_child_ctx_id;
    }

    void sendToSelf(JobMessage& msg, int mpiTag = MSG_SEND_APPLICATION_MESSAGE) const {
        msg.treeIndexOfDestination = getIndex();
        msg.contextIdOfDestination = _ctx_id;
        this->send(getRank(), mpiTag, msg);
    }
    void sendToRoot(JobMessage& msg, int mpiTag = MSG_SEND_APPLICATION_MESSAGE) const {
        msg.treeIndexOfDestination = 0;
        msg.contextIdOfDestination = getRootContextId();
        this->send(getRootNodeRank(), mpiTag, msg);
    }
    void sendToParent(JobMessage& msg, int mpiTag = MSG_SEND_APPLICATION_MESSAGE) const {
        msg.treeIndexOfDestination = getParentIndex();
        msg.contextIdOfDestination = getParentContextId();
        this->send(getParentNodeRank(), mpiTag, msg);
    }
    void sendToAnyChildren(JobMessage& msg, int mpiTag = MSG_SEND_APPLICATION_MESSAGE) const {
        if (hasLeftChild()) sendToLeftChild(msg, mpiTag);
        if (hasRightChild()) sendToRightChild(msg, mpiTag);
    }
    void sendToLeftChild(JobMessage& msg, int mpiTag = MSG_SEND_APPLICATION_MESSAGE) const {
        assert(hasLeftChild());
        msg.treeIndexOfDestination = getLeftChildIndex();
        msg.contextIdOfDestination = getLeftChildContextId();
        this->send(getLeftChildNodeRank(), mpiTag, msg);
    }
    void sendToRightChild(JobMessage& msg, int mpiTag = MSG_SEND_APPLICATION_MESSAGE) const {
        assert(hasRightChild());
        msg.treeIndexOfDestination = getRightChildIndex();
        msg.contextIdOfDestination = getRightChildContextId();
        this->send(getRightChildNodeRank(), mpiTag, msg);
    }

    void addSendHandle(int dest, int sendId) {
        if (dest == getLeftChildNodeRank())
            _send_handles_left.push_back(sendId);
        else if (dest == getRightChildNodeRank()) {
            _send_handles_right.push_back(sendId);
        }
    }
    void clearSendHandle(int sendId) {
        auto it = _send_handles_left.begin();
        while (it != _send_handles_left.end()) {
            if (*it == sendId) {
                _send_handles_left.erase(it);
                return;
            }
            ++it;
        }
        it = _send_handles_right.begin();
        while (it != _send_handles_right.end()) {
            if (*it == sendId) {
                _send_handles_right.erase(it);
                return;
            }
            ++it;
        }
    }
    void unsetLeftChild() {
        if (!hasLeftChild()) return; 
        int rank = getLeftChildNodeRank();
        _past_children.insert(rank);
        addDormantChild(rank);
        _left_child_ctx_id = 0;
        for (int id : _send_handles_left) {
            MyMpi::getMessageQueue().cancelSend(id);
        }
        _send_handles_left.clear();
        _job_node_ranks.clear(getLeftChildIndex());
    }
    void unsetRightChild() {
        if (!hasRightChild()) return;
        int rank = getRightChildNodeRank();
        _past_children.insert(rank); 
        addDormantChild(rank);
        _right_child_ctx_id = 0;
        for (int id : _send_handles_right) {
            MyMpi::getMessageQueue().cancelSend(id);
        }
        _send_handles_right.clear();
        _job_node_ranks.clear(getRightChildIndex());
    }
    void update(int index, int rootRank, ctx_id_t rootContextId, int parentRank, ctx_id_t parentContextId) {    
        if (hasLeftChild()) unsetLeftChild();
        if (hasRightChild()) unsetRightChild();
        _index = index;
        if (index == 0 || rootRank < 0) rootRank = _rank; // this is the root node
        updateJobNode(0, rootRank);
        updateJobNode(_index, _rank);
        updateParentNodeRank(parentRank);
        if (index == 0) _root_ctx_id = getContextId();
        else _root_ctx_id = rootContextId;
        assert(_root_ctx_id != 0);
        _parent_ctx_id = parentContextId;
        assert(index == 0 || _parent_ctx_id != 0);
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
    void addDormantChild(int rank) {
        if (!_use_dormant_children) return;
        removeDormantChild(rank);
        _dormant_children.insert(_it_dormant_children, rank);
    }
    void removeDormantChild(int rank) {
        if (_dormant_children.empty()) return;
        int currentRank = *_it_dormant_children;
        for (auto it = _dormant_children.begin(); it != _dormant_children.end(); ++it) {
            if (*it == rank) {
                it = _dormant_children.erase(it);
                it--;
            }
        }
        // Properly reset current iterator
        _it_dormant_children = _dormant_children.begin();
        for (auto it = _dormant_children.begin(); it != _dormant_children.end(); ++it) {
            if (*it == currentRank) {
                _it_dormant_children = it;
                break;
            }   
        }
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

    void setDesireLeft(float time) {setDesire(_time_of_desire_left, time);}
    void setDesireRight(float time) {setDesire(_time_of_desire_right, time);}
    void unsetDesireLeft() {_time_of_desire_left = -1;}
    void unsetDesireRight() {_time_of_desire_right = -1;}

    float getNumDesires() const {return _num_desires;}
    float getNumFulfiledDesires() const {return _num_fulfilled_desires;}
    float getSumOfDesireLatencies() const {return _sum_desire_latencies;}
    std::vector<float>& getDesireLatencies() {return _desire_latencies;}
    
    void setWaitingForReactivation(int epoch) {
        LOG(V5_DEBG, "RBS WAIT\n");
        _wait_epoch = std::max(_wait_epoch, epoch);
    }
    void stopWaitingForReactivation(int epoch) {
        if (_stop_wait_epoch < epoch) LOG(V5_DEBG, "RBS STOPWAIT\n");
        _stop_wait_epoch = std::max(_stop_wait_epoch, epoch);
    }
    bool isWaitingForReactivation() const {
        return _wait_epoch > _stop_wait_epoch;
    }

    int getNumChildren() const {
        int numChildren = 0;
        if (hasLeftChild()) numChildren++;
        if (hasRightChild()) numChildren++;
        return numChildren;
    }

    JobTreeSnapshot getSnapshot() const {
        JobTreeSnapshot snapshot;
        snapshot.nodeRank = getRank();
        snapshot.index = getIndex();
        snapshot.contextId = getContextId();
        snapshot.nbChildren = getNumChildren();
        snapshot.leftChildNodeRank = hasLeftChild() ? getLeftChildNodeRank() : -1;
        snapshot.leftChildIndex = hasLeftChild() ? getLeftChildIndex() : -1;
        snapshot.leftChildContextId = hasLeftChild() ? getLeftChildContextId() : 0;
        snapshot.rightChildNodeRank = hasRightChild() ? getRightChildNodeRank() : -1;
        snapshot.rightChildIndex = hasRightChild() ? getRightChildIndex() : -1;
        snapshot.rightChildContextId = hasRightChild() ? getRightChildContextId() : 0;
        snapshot.parentNodeRank = getParentNodeRank();
        snapshot.parentIndex = getParentIndex();
        snapshot.parentContextId = getParentContextId();
        return snapshot;
    }

private:
    void send(int dest, int mpiTag, JobMessage& msg) const {
        msg.treeIndexOfSender = getIndex();
        msg.contextIdOfSender = _ctx_id;
        MyMpi::isend(dest, mpiTag, msg);
    }

    void setDesire(float& member, float time) {
        if (member == -1) {
            // new desire
            member = time;
            _num_desires++;
        }
    }
    void fulfilDesireLeft(float time) {fulfilDesire(_time_of_desire_left, time);}
    void fulfilDesireRight(float time) {fulfilDesire(_time_of_desire_right, time);}
    void fulfilDesire(float& member, float time) {
        if (member == -1) return;
        _num_fulfilled_desires++;
        auto elapsed = time - member;
        _sum_desire_latencies += elapsed;
        _desire_latencies.push_back(elapsed);
        if (elapsed > 0.1) {
            LOG(V4_VVER, "LATENCY %.4f born: %.4f\n", elapsed, member);
        }
        member = -1; // no desire any more
    }

    static int getLeftChildIndex(int index) {return 2*(index+1)-1;}
    static int getRightChildIndex(int index) {return 2*(index+1);}
    static int getParentIndex(int index) {return (index-1)/2;}    

};

#endif