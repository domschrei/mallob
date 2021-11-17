
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
    JobTree& _job_tree;

    std::function<void(int, int, int)> _cb_emit_directed_job_request;
    std::function<void(int, int)> _cb_emit_undirected_job_request;

    std::vector<std::unique_ptr<ChildInterface>> _sessions;
    std::unique_ptr<ChildInterface> _empty_session;
    int _epoch_of_last_suspension = -1;

    int _last_update_epoch = -1;
    int _last_update_volume = -1;

    bool _suspending = false;
    bool _suspended = true;

public:
    LocalScheduler(int jobId, const Parameters& params, JobTree& jobTree, 
            std::function<void(int, int, int)> cbEmitDirectedJobRequest, 
            std::function<void(int, int)> cbEmitUndirectedJobRequest) 
        : _job_id(jobId), _params(params), _job_tree(jobTree), 
            _cb_emit_directed_job_request(cbEmitDirectedJobRequest),
            _cb_emit_undirected_job_request(cbEmitUndirectedJobRequest) {
        _sessions.resize(2);
    }

    /*
    Receive a job scheduling update with appropriately scoped list of inactive job nodes.
    Initialize two sessions:
    - set local inactive nodes to the ones in the update
    - update epoch, desired volume
    - (!has && wants): begin negotiation procedure using inactive job nodes
    - (has && wants): forward update to child
    */
    void initializeScheduling(const JobSchedulingUpdate& update) {

        if (update.epoch <= _epoch_of_last_suspension) {
            // Past update: just send it back
            int parentRank = _job_tree.getParentNodeRank();        
            MyMpi::isend(parentRank, MSG_SCHED_RETURN_NODES, update);
            return;
        }
        if (update.volume >= 0 && update.epoch > _last_update_epoch) {
            _last_update_epoch = update.epoch;
            _last_update_volume = update.volume;
        }

        _suspended = false;
        auto [leftUpdate, rightUpdate] = update.split(_job_tree.getIndex());
        for (size_t i = 0; i < 2; i++) {
            auto& session = _sessions[i];
            auto& childUpdate = i==0 ? leftUpdate : rightUpdate;
            session.reset(new ChildInterface(_job_id, _job_tree, std::move(childUpdate.inactiveJobNodes), 
                    i == 0 ? _job_tree.getLeftChildIndex() : _job_tree.getRightChildIndex(), 
                    i == 0 ? _job_tree.getLeftChildNodeRank() : _job_tree.getRightChildNodeRank()));
            if (_last_update_epoch >= 0) {
                auto directive = session->handleBalancingUpdate(_last_update_epoch, _last_update_volume);
                applyDirective(directive, session);
            }
        }
    }

    // called from local balancer update
    void updateBalancing(int epoch, int volume) {

        log(V5_DEBG, "RBS #%i:%i BLC e=%i\n", _job_id, _job_tree.getIndex(), epoch);
        if (_last_update_epoch >= epoch) return;
        _last_update_epoch = epoch;
        _last_update_volume = volume;

        for (auto& session : _sessions) {
            if (!session) continue;
            auto directive = session->handleBalancingUpdate(epoch, volume);
            applyDirective(directive, session);
        }
        
        if (volume <= _job_tree.getIndex()) {
            // Suspend (if not already suspended), remember this epoch as most recent suspension
            _epoch_of_last_suspension = epoch;
            if (!_suspended) {
                _suspending = true;
                if (canReturnInactiveJobNodes()) returnInactiveJobNodesToParent();
            }
        }
    }

    void handle(MessageHandle& h) {
        
        if (h.tag == MSG_SCHED_INITIALIZE_CHILD_WITH_NODES) {

            // Message from parent with job nodes
            JobSchedulingUpdate update;
            update.deserialize(h.getRecvData());
            initializeScheduling(update);

        } else if (h.tag == MSG_SCHED_RETURN_NODES) {
            
            // Message from parent to all applicable sessions
            JobSchedulingUpdate update;
            update.deserialize(h.getRecvData());
            auto& session = getSessionByChildRank(h.source);
            if (session) session->addJobNodesFromSuspendedChild(h.source, update.inactiveJobNodes);
        }

        if (canReturnInactiveJobNodes()) returnInactiveJobNodesToParent();
    }

    void handleRejectReactivation(int source, int epoch, int index, bool lost) {
        
        auto& session = getSessionByChildIndex(index);
        if (!session) return;
        auto directive = session->handleRejectionOfPotentialChild(index, epoch, lost);
        applyDirective(directive, session);
    }

    void handleChildJoining(int source, int epoch, int index) {
        
        auto& session = getSessionByChildIndex(index);
        if (!session) return;
        log(V5_DEBG, "RBS Child %i found\n", source);
        session->handleChildJoining(source, index, epoch);
    }

    /*
    A suspending child transfers inactive job nodes to its parent.
    In the session:
    - Store in local inactive job nodes
    If scheduler is suspended and all children have reported: 
      send up all local inactive job nodes(including yourself) to the parent
    If "new" children (maybe the same ranks) are present and have not yet received scheduling update:
      send scheduling update down to the children
    */
    void handleChildReturningInactiveJobNodes(int childRank, const InactiveJobNodeList& nodes) {
        auto& session = getSessionByChildRank(childRank);
        if (!session) return;
        session->addJobNodesFromSuspendedChild(childRank, nodes);
    }

    /*
    True iff received nodes from child (if it was present) and handleSuspend() was called. 
    */
    bool canReturnInactiveJobNodes() {
        if (!_suspended && _suspending) {
            auto& left = _sessions[0];
            auto& right = _sessions[1];
            if (!(left->doesChildHaveNodes()) && !(right->doesChildHaveNodes())) {
                return true;
            }
        }
        return false;
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
        int epoch = std::max(_sessions[0]->getEpoch(), _sessions[1]->getEpoch());
        auto myNode = InactiveJobNode(MyMpi::rank(MPI_COMM_WORLD), _job_tree.getIndex(), epoch);
        myNode.status = InactiveJobNode::AVAILABLE;
        nodes.set.insert(myNode);
        
        // Consolidate set of inactive nodes (discarding lost ones)
        nodes.cleanUpStatuses();

        // Send inactive job nodes to parent
        JobSchedulingUpdate update;
        update.jobId = _job_id;
        update.inactiveJobNodes = std::move(nodes);
        int parentRank = _job_tree.getParentNodeRank();        
        MyMpi::isend(parentRank, MSG_SCHED_RETURN_NODES, update);

        // Update internal state (discard current child interfaces)
        _suspending = false;
        _suspended = true;
        _sessions.clear();
        _sessions.resize(2);
    }

    bool canCommit() const {
        return _suspended;
    }

    bool acceptsChild(int index) {
        auto& session = getSessionByChildIndex(index);
        return session && !session->hasChild() && session->wantsChild();
    }

    void resetRole() {
        _epoch_of_last_suspension = -1;
        _last_update_epoch = -1;
    }

private:

    void applyDirective(ChildInterface::MsgDirective directive, std::unique_ptr<ChildInterface>& session) {
        if (directive == ChildInterface::EMIT_DIRECTED_REQUEST)
            _cb_emit_directed_job_request(session->getEpoch(), session->getChildIndex(), session->getChildRank());
        if (directive == ChildInterface::EMIT_UNDIRECTED_REQUEST) 
            _cb_emit_undirected_job_request(session->getEpoch(), session->getChildIndex());
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
