
#pragma once

#include <functional>
#include <assert.h>

#include "app/job_tree.hpp"
#include "job_scheduling_update.hpp"
#include "comm/mympi.hpp"
#include "util/logger.hpp"
#include "session.hpp"

class LocalScheduler {

private:
    int _job_id;
    const Parameters& _params;
    int _index;
    int _parent_rank;
    int _parent_index;
    int _left_child_index;
    int _right_child_index;

    std::function<void(int epoch, bool left, int dest)> _cb_emit_request;

    std::vector<std::unique_ptr<ChildInterface>> _sessions;
    std::unique_ptr<ChildInterface> _empty_session;
    int _epoch_of_last_suspension = -1;

    int _last_update_epoch = -1;
    int _last_update_volume = -1;

    bool _suspending = false;
    bool _suspended = true;
    bool _resuming = false;

public:
    LocalScheduler(int jobId, const Parameters& params, JobTree& jobTree,
            std::function<void(int epoch, bool left, int dest)> cbEmitRequest) 
        : _job_id(jobId), _params(params), _index(jobTree.getIndex()), 
            _parent_rank(jobTree.getParentNodeRank()), _parent_index(jobTree.getParentIndex()), 
            _left_child_index(jobTree.getLeftChildIndex()), _right_child_index(jobTree.getRightChildIndex()),
            _cb_emit_request(cbEmitRequest) {
        _sessions.resize(2);
        LOG(V5_DEBG, "RBS OPEN #%i:%i\n", _job_id, _index);
    }
    LocalScheduler(LocalScheduler&& other) : 
        _job_id(other._job_id), _params(other._params),
        _cb_emit_request(other._cb_emit_request),
        _sessions(std::move(other._sessions)), _empty_session(std::move(other._empty_session)),
        _epoch_of_last_suspension(other._epoch_of_last_suspension), 
        _last_update_epoch(other._last_update_epoch), _last_update_volume(other._last_update_volume), 
        _suspending(other._suspending), _suspended(other._suspended) {

        _index = other._index;
        _parent_rank = other._parent_rank;
        _parent_index = other._parent_index;
        _left_child_index = other._left_child_index;
        _right_child_index = other._right_child_index;
    }
    ~LocalScheduler() {
        LOG(V5_DEBG, "RBS CLOSE #%i:%i\n", _job_id, _index);
    }

    /*
    Receive a job scheduling update with appropriately scoped list of inactive job nodes.
    Initialize two sessions:
    - set local inactive nodes to the ones in the update
    - update epoch, desired volume
    - (!has && wants): begin negotiation procedure using inactive job nodes
    - (has && wants): forward update to child
    */
    void initializeScheduling(JobSchedulingUpdate& update, int parentRank) {

        _parent_rank = parentRank;

        if (_index > 0 && update.epoch <= _epoch_of_last_suspension) {
            // Past update: just send it back
            update.destinationIndex = _parent_index;
            MyMpi::isend(_parent_rank, MSG_SCHED_RETURN_NODES, update);
            return;
        }
        if (update.volume >= 0 && update.epoch > _last_update_epoch) {
            _last_update_epoch = update.epoch;
            _last_update_volume = update.volume;
        }

        _suspended = false;
        auto [leftUpdate, rightUpdate] = update.split(_index);
        for (size_t i = 0; i < 2; i++) {
            auto& session = _sessions[i];
            assert(!session);
            auto& childUpdate = i==0 ? leftUpdate : rightUpdate;
            session.reset(new ChildInterface(_job_id, std::move(childUpdate.inactiveJobNodes), 
                    i == 0 ? _left_child_index : _right_child_index));
            if (_last_update_epoch >= 0) {
                auto directive = session->handleBalancingUpdate(_last_update_epoch, _last_update_volume, 
                    /*hasChild=*/false);
                applyDirective(directive, session);
            }
        }
    }

    // called from local balancer update
    void updateBalancing(int epoch, int volume, bool hasLeftChild, bool hasRightChild) {

        LOG(V5_DEBG, "RBS #%i:%i BLC e=%i\n", _job_id, _index, epoch);
        if (_last_update_epoch >= epoch) return;
        _last_update_epoch = epoch;
        _last_update_volume = volume;
        
        if (_suspending || _suspended) return;

        for (size_t i = 0; i < _sessions.size(); i++) {
            auto& session = _sessions[i];
            if (!session) continue;
            auto directive = session->handleBalancingUpdate(epoch, volume, i==0 ? hasLeftChild : hasRightChild);
            applyDirective(directive, session);
        }
        
        if (volume <= _index && _index > 0) beginSuspension(epoch);
    }

    void beginSuspension(int epoch = -1) {
        LOG(V5_DEBG, "RBS #%i:%i SUSPEND\n", _job_id, _index);
        if (epoch == -1) epoch = std::max(_last_update_epoch, _epoch_of_last_suspension);
        // Suspend (if not already suspended), remember this epoch as most recent suspension
        _epoch_of_last_suspension = epoch;
        if (!_suspended) {
            _suspending = true;
            if (canReturnInactiveJobNodes()) returnInactiveJobNodesToParent();
        }
    }

    void handle(MessageHandle& h) {
        
        if (h.tag == MSG_SCHED_INITIALIZE_CHILD_WITH_NODES) {

            // Message from parent with job nodes
            JobSchedulingUpdate update;
            update.deserialize(h.getRecvData());
            initializeScheduling(update, h.source);

        } else if (h.tag == MSG_SCHED_RETURN_NODES) {
            
            // Message from parent to all applicable sessions
            JobSchedulingUpdate update;
            update.deserialize(h.getRecvData());
            auto& session = getSessionByChildRank(h.source);
            if (session) session->addJobNodesFromSuspendedChild(h.source, update.inactiveJobNodes);
        }

        if (canReturnInactiveJobNodes()) returnInactiveJobNodesToParent();
        if (canResumeAsRoot()) resumeAsRoot();
    }

    void handleRejectReactivation(int source, int epoch, int index, bool lost, bool hasChild) {
        
        auto& session = getSessionByChildIndex(index);
        if (!session) return;
        auto directive = session->handleRejectionOfPotentialChild(index, epoch, lost, 
            hasChild, _suspended || _suspending);
        applyDirective(directive, session);
    }

    void handleChildJoining(int source, int epoch, int index) {
        
        auto& session = getSessionByChildIndex(index);
        if (!session) return;
        LOG(V5_DEBG, "RBS Child %i found\n", source);
        session->handleChildJoining(source, index, epoch);
    }

    void handleNodeFreed(int rank, int epoch, int index) {
        for (auto& session : _sessions) {
            if (!session) continue;
            session->removeNode(rank, epoch, index);
        }
    }

    void beginResumptionAsRoot() {
        assert(_index == 0);
        LOG(V5_DEBG, "RBS #%i:%i RESUMING\n", _job_id, _index);
        assert(_suspending && !_suspended);
        _resuming = true;
        if (canResumeAsRoot()) resumeAsRoot();
    }

    void resumeAsRoot() {
        LOG(V5_DEBG, "RBS #%i:%i RESUME\n", _job_id, _index);
        assert(canResumeAsRoot());
        _resuming = false;
        _suspending = false;

        JobSchedulingUpdate update;
        update.epoch = std::max(_last_update_epoch, _epoch_of_last_suspension);
        update.volume = _last_update_volume;
        update.inactiveJobNodes.set.clear();
        update.inactiveJobNodes.mergePreferringNewer(_sessions[0]->returnJobNodes());
        update.inactiveJobNodes.mergePreferringNewer(_sessions[1]->returnJobNodes());
        update.inactiveJobNodes.cleanUpStatuses();
        
        _sessions.clear();
        _sessions.resize(2);

        initializeScheduling(update, _parent_rank);
    }

    /*
    True iff received nodes from child (if it was present) and handleSuspend() was called. 
    */
    bool canReturnInactiveJobNodes() {
        if (_index == 0) return false;
        if (!_suspended && _suspending) {
            auto& left = _sessions[0];
            auto& right = _sessions[1];
            if (!(left->doesChildHaveNodes()) && !(right->doesChildHaveNodes())) {
                return true;
            }
        }
        return false;
    }

    bool canResumeAsRoot() {
        return _resuming && !_sessions[0]->doesChildHaveNodes() && !_sessions[1]->doesChildHaveNodes();
    }

    /*
    Following a suspension, return the local inactive job nodes to the parent.
    */
    void returnInactiveJobNodesToParent() {

        assert(_suspending);
        assert(!_suspended);

        // Merge inactive job nodes of both children
        InactiveJobNodeList nodes;
        nodes.mergePreferringNewer(_sessions[0]->returnJobNodes());
        nodes.mergePreferringNewer(_sessions[1]->returnJobNodes());

        // Add yourself as an inactive job node
        int epoch = _last_update_epoch;
        auto myNode = InactiveJobNode(MyMpi::rank(MPI_COMM_WORLD), _index, epoch);
        myNode.status = InactiveJobNode::AVAILABLE;
        nodes.set.insert(myNode);
        
        // Consolidate set of inactive nodes (discarding lost ones)
        nodes.cleanUpStatuses();

        // Send inactive job nodes to parent
        JobSchedulingUpdate update;
        update.jobId = _job_id;
        update.destinationIndex = _parent_index;
        update.inactiveJobNodes = std::move(nodes);       
        MyMpi::isend(_parent_rank, MSG_SCHED_RETURN_NODES, update);

        // Update internal state (discard current child interfaces)
        _suspending = false;
        _suspended = true;
        _sessions.clear();
        _sessions.resize(2);
    }

    bool isActive() const {
        return !_suspended && !_suspending;
    }

    bool canCommit() const {
        return _suspended;
    }

    bool acceptsChild(int index) {
        auto& session = getSessionByChildIndex(index);
        if (!session) {
            LOG(V5_DEBG, "RBS child idx=%i not present\n", index);
            return false;
        }
        if (!session->wantsChild()) {
            LOG(V5_DEBG, "RBS child idx=%i wants no child\n", index);
            return false;
        }
        return true;
    }

    void resetRole() {
        _epoch_of_last_suspension = -1;
        _last_update_epoch = -1;
        _sessions.clear();
        _sessions.resize(2);
    }

private:

    void applyDirective(ChildInterface::MsgDirective directive, std::unique_ptr<ChildInterface>& session) {
        if (directive == ChildInterface::DO_NOTHING) return;
        bool isDirected = directive == ChildInterface::EMIT_DIRECTED_REQUEST;
        _cb_emit_request(session->getEpoch(), session->getChildIndex() == _left_child_index, 
                        isDirected ? session->getChildRank() : -1);
    }

    std::unique_ptr<ChildInterface>& getSessionByChildIndex(int childIndex) {
        if (_sessions[0] && childIndex == _sessions[0]->getChildIndex()) return _sessions[0];
        if (_sessions[1] && childIndex == _sessions[1]->getChildIndex()) return _sessions[1];
        return _empty_session;
    }
    std::unique_ptr<ChildInterface>& getSessionByChildRank(int childRank) {
        if (_sessions[0] && childRank == _sessions[0]->getChildRank()) return _sessions[0];
        if (_sessions[1] && childRank == _sessions[1]->getChildRank()) return _sessions[1];
        return _empty_session;
    }
    std::unique_ptr<ChildInterface>& other(const std::unique_ptr<ChildInterface>& session) {
        return session == _sessions[0] ? _sessions[1] : _sessions[0];
    }
};
