
#pragma once

#include <functional>
#include <assert.h>

#include "app/job_tree.hpp"
#include "job_scheduling_update.hpp"
#include "comm/mympi.hpp"

class LocalScheduler {

private:
    struct Session {

        enum State {
            PROPAGATE_WAITING_FOR_CHILD,
            PROPAGATE_WAITING_FOR_PARENT,
            NEGOTIATE_WAITING_FOR_CHILD,
            NEGOTIATE_WAITING_FOR_PARENT,
            DONE
        };

        enum Result {
            OK,
            REQUEST_INACTIVE_NODES,
            CONCLUDE,
            CONCLUDE_EMIT_JOB_REQUEST
        };

        JobTree& tree;
        JobSchedulingUpdate update;
        State state;

        int epoch = -1;
        int childIndex;
        int childRank = -1;
        int numQueriedJobNodes = 0;
        bool exportedExcessNodes = false;

        Session(JobTree& tree) : tree(tree) {}

        void init(int epoch, int childIndex, int childRank, bool wantsChild, bool hasChild) {
            if (epoch == this->epoch) return; // already at this epoch
            this->epoch = epoch;
            this->childIndex = childIndex;
            this->childRank = childRank;
            if (!hasChild && wantsChild) state = NEGOTIATE_WAITING_FOR_PARENT;
            else if (hasChild && wantsChild) state = PROPAGATE_WAITING_FOR_PARENT;
            else state = DONE;
            this->exportedExcessNodes = false;
        }

        // from parent
        Result handlePropagateSchedulingUpdate(const JobSchedulingUpdate& update) {
            if (state == PROPAGATE_WAITING_FOR_PARENT) {
                // Propagate inactive job nodes down to child.
                // Prev. update can be overwritten: No need for prior inactive job nodes any more,
                // final set of inactive job nodes will be returned by child
                this->update = update;
                MyMpi::isend(childRank, MSG_SCHED_PROPAGATE, update);
                state = PROPAGATE_WAITING_FOR_CHILD;
                return OK;
            }
            if (state == NEGOTIATE_WAITING_FOR_PARENT) {
                // Process inactive job nodes and query potential children.
                this->update.inactiveJobNodes.merge(update.inactiveJobNodes);
                this->update.inactiveJobNodes.initIterator();
                return recruitChild();
            }
            assert(state == DONE);
            return CONCLUDE;
        }

        // from parent
        Result handleNotifyNoneLeft() {
            assert(state == PROPAGATE_WAITING_FOR_PARENT || state == NEGOTIATE_WAITING_FOR_PARENT);
            if (state == PROPAGATE_WAITING_FOR_PARENT) {
                notifyChildNoneLeft();
                state = PROPAGATE_WAITING_FOR_CHILD;
                return OK;
            }
            if (state == NEGOTIATE_WAITING_FOR_PARENT) {
                state = DONE;
                return CONCLUDE_EMIT_JOB_REQUEST;
            }
            assert(state == DONE);
            return OK;
        }

        // from child
        Result handleRequestInactiveJobNodes() {
            assert(state == PROPAGATE_WAITING_FOR_CHILD);
            state = PROPAGATE_WAITING_FOR_PARENT;
            return REQUEST_INACTIVE_NODES;
        }

        // from potential child
        Result handleRejectReactivate(bool lost) {
            if (lost) {
                update.inactiveJobNodes.it->status = InactiveJobNode::LOST;
            } else {
                update.inactiveJobNodes.it->status = InactiveJobNode::BUSY;
            }
            update.inactiveJobNodes.it++;
            return recruitChild();
        }

        // from potential (->actual) child
        Result handleConfirmReactivate() {
            assert(state == NEGOTIATE_WAITING_FOR_CHILD);
            MyMpi::isend(childRank, MSG_SCHED_PROPAGATE, update);
            state = PROPAGATE_WAITING_FOR_CHILD;
            update.inactiveJobNodes.it->status = InactiveJobNode::BUSY;
            return OK;
        }

        // from child
        Result handleConclude(const JobSchedulingUpdate& update) {
            assert(state == PROPAGATE_WAITING_FOR_CHILD);
            state = DONE;
            this->update.inactiveJobNodes.merge(update.inactiveJobNodes);
            return CONCLUDE;
        }

        bool canConsumeInactiveJobNodes() const {
            return state == PROPAGATE_WAITING_FOR_PARENT || state == NEGOTIATE_WAITING_FOR_PARENT;
        }

        void setExcessNodesExported() {
            exportedExcessNodes = true;
        }

        bool hasExcessNodesReady() const {
            return !exportedExcessNodes && state == DONE && update.inactiveJobNodes.containsUsableNodes();
        }

        bool isDone() const {
            return state == DONE;
        }


private:
        /* --- helpers --- */
        
        Result recruitChild() {
            auto& it = update.inactiveJobNodes.it;
            for (; numQueriedJobNodes < 4 && it != update.inactiveJobNodes.set.end(); it++) {
                auto& node = *it;
                if (node.rank < 0 || node.rank >= MyMpi::size(MPI_COMM_WORLD)) {
                    // Node is known to be lost or busy: do not query
                    continue;
                }
                // Query node for adoption
                childRank = node.rank;
                IntPair payload(update.jobId, update.epoch);
                MyMpi::isend(childRank, MSG_SCHED_REQUEST_REACTIVATE, payload);
                tree.updateJobNode(childIndex, childRank);
                numQueriedJobNodes++;
                state = NEGOTIATE_WAITING_FOR_CHILD;
                return OK; // wait for response 
            }

            // No inactive job nodes left!
            if (numQueriedJobNodes >= 4) {
                // Queried too many job nodes: "fail"
                state = DONE;
                // TODO notify CA
                return CONCLUDE_EMIT_JOB_REQUEST;
            } else {
                // Query nodes from parent
                state = NEGOTIATE_WAITING_FOR_PARENT;
                return REQUEST_INACTIVE_NODES;
            }
        }

        void notifyChildNoneLeft() {
            assert(state == PROPAGATE_WAITING_FOR_PARENT);
            MyMpi::isend(childRank, MSG_SCHED_NOTIFY_NONE_LEFT, IntPair(update.jobId, update.epoch));
            state = PROPAGATE_WAITING_FOR_CHILD;
        }
    };

    int _job_id;
    Parameters& _params;
    JobTree& _job_tree;
    std::function<void(int)> _cb_emit_job_request;

    InactiveJobNodeList _inactive_job_nodes;
    Session _session_left;
    Session _session_right;

public:
    LocalScheduler(int jobId, Parameters& params, JobTree& jobTree, std::function<void(int)> cbEmitJobRequest) 
        : _job_id(jobId), _params(params), _job_tree(jobTree), _cb_emit_job_request(cbEmitJobRequest),
            _session_left(_job_tree), _session_right(_job_tree) {}

    void initiateSchedulingUpdate(int epoch, int volume, int difference) {
        JobSchedulingUpdate update(_job_id, epoch, volume, difference, std::move(_inactive_job_nodes));
        handleJobSchedulingUpdate(update);
        _inactive_job_nodes.set.clear();
    }

    void handle(MessageHandle& h) {
        if (h.tag == MSG_SCHED_NEW_INACTIVE_NODE) {
            // Report of a new inactive job node
            InactiveJobNodeList list = Serializable::get<InactiveJobNodeList>(h.getRecvData());
            _inactive_job_nodes.set.insert(list.set.begin(), list.set.end());
        } else if (h.tag == MSG_SCHED_PROPAGATE) {
            // Message from parent creating sessions
            JobSchedulingUpdate update;
            update.deserialize(h.getRecvData());
            handleJobSchedulingUpdate(update);
        } else if (h.tag == MSG_SCHED_NOTIFY_NONE_LEFT) {
            // Message from parent to all applicable sessions
            if (_session_left.canConsumeInactiveJobNodes()) {
                auto result = _session_left.handleNotifyNoneLeft();
                handleResult(_session_left, result, _session_right);
            }
            if (_session_right.canConsumeInactiveJobNodes()) {
                auto result = _session_right.handleNotifyNoneLeft();
                handleResult(_session_right, result, _session_left);
            }
        } else {
            IntVec payload = Serializable::get<IntVec>(h.getRecvData());
            int jobId = payload[0];
            int epoch = payload[1];
            // Message from a (potential?) child to a specific session
            auto& session = h.source == _session_left.childRank ? _session_left : _session_right;
            auto& otherSession = h.source == _session_left.childRank ? _session_right : _session_left;
            Session::Result result;
            assert(h.source == session.childRank);
            if (h.tag == MSG_SCHED_CONFIRM_REACTIVATE) {
                result = session.handleConfirmReactivate();
            } else if (h.tag == MSG_SCHED_REJECT_REACTIVATE) {
                bool lost = payload[2];
                result = session.handleRejectReactivate(lost);
            } else if (h.tag == MSG_SCHED_REQUEST_INACTIVE_NODES) {
                result = session.handleRequestInactiveJobNodes();
            } else if (h.tag == MSG_SCHED_CONCLUDE) {
                JobSchedulingUpdate update;
                update.deserialize(h.getRecvData());
                result = session.handleConclude(update);
            }
            handleResult(session, result, otherSession);
        }
    }
    
    /*
    For each child position (left, right):
    If present: split, broadcast down
    If not present: handle split section yourself
    Handling split sections: digestJobSchedulingUpdate
    */
    void handleJobSchedulingUpdate(const JobSchedulingUpdate& update) {
        int vol = update.volume;
        bool wantsLeft = _job_tree.getLeftChildIndex() < vol;
        bool wantsRight = _job_tree.getRightChildIndex() < vol;

        // Initialize sessions with new epoch, if necessary
        _session_left.init(update.epoch, _job_tree.getLeftChildIndex(), _job_tree.getLeftChildNodeRank(), 
            wantsLeft, _job_tree.hasLeftChild());
        _session_right.init(update.epoch, _job_tree.getRightChildIndex(), _job_tree.getRightChildNodeRank(), 
            wantsRight, _job_tree.hasRightChild());

        // Check who is eligible to consume the inactive job nodes in the update
        Session::Result resultLeft, resultRight;
        if (_session_left.canConsumeInactiveJobNodes() && _session_right.canConsumeInactiveJobNodes()) {
            // Two child positions to care for: split up the update
            auto [leftUpdate, rightUpdate] = update.split(_job_tree.getIndex());
            resultLeft = _session_left.handlePropagateSchedulingUpdate(leftUpdate);
            resultRight = _session_right.handlePropagateSchedulingUpdate(rightUpdate);
            handleResult(_session_left, resultLeft, _session_right);
            handleResult(_session_right, resultRight, _session_left);
        } else if (_session_left.canConsumeInactiveJobNodes()) {
            resultLeft = _session_left.handlePropagateSchedulingUpdate(update);
            handleResult(_session_left, resultLeft, _session_right);
        } else if (_session_right.canConsumeInactiveJobNodes()) {
            resultRight = _session_right.handlePropagateSchedulingUpdate(update);
            handleResult(_session_right, resultRight, _session_left);
        }
    }

    void handleResult(Session& session, Session::Result result, Session& otherSession) {
        if (result == Session::OK) return;
        bool sessionDone = false;
        if (result == Session::CONCLUDE) sessionDone = true;
        if (result == Session::CONCLUDE_EMIT_JOB_REQUEST) {
            _cb_emit_job_request(session.childIndex);
            sessionDone = true;
        }
        if (result == Session::REQUEST_INACTIVE_NODES) {
            // wants to have excess nodes
            if (otherSession.hasExcessNodesReady()) {
                // TODO Transfer nodes from other session to this session
                session.handlePropagateSchedulingUpdate(otherSession.update);
                otherSession.setExcessNodesExported();
            } else {
                // TODO wait for other session (?)
                IntVec payload;
                payload.data.push_back(_job_id);
                payload.data.push_back(session.epoch);
                if (_job_tree.isRoot()) {
                    // No more inactive job nodes left: Send notification down
                    MyMpi::isend(session.childRank, MSG_SCHED_NOTIFY_NONE_LEFT, payload);
                } else {
                    // Query parent for more nodes
                    MyMpi::isend(_job_tree.getParentNodeRank(), MSG_SCHED_REQUEST_INACTIVE_NODES, payload);
                }
            }
        }
        if (sessionDone && session.hasExcessNodesReady() && otherSession.canConsumeInactiveJobNodes()) {
            // Transfer nodes from this session to other session
            otherSession.handlePropagateSchedulingUpdate(session.update);
            session.setExcessNodesExported();

        } else if (sessionDone && otherSession.isDone()) {
            // Both sessions are done 
            session.update.inactiveJobNodes.merge(otherSession.update.inactiveJobNodes);
            if (_job_tree.isRoot()) {
                // Job scheduling phase finished: Remember inactive job nodes
                _inactive_job_nodes = std::move(session.update.inactiveJobNodes);
                _inactive_job_nodes.cleanUp(); // remove lost inactive job nodes, reset busy ones
            } else {
                // Send up excess nodes from both
                MyMpi::isend(_job_tree.getParentNodeRank(), MSG_SCHED_CONCLUDE, session.update);
            }
        }
    }

    void advance() {
        if (!_inactive_job_nodes.set.empty() && !_job_tree.isRoot()) {
            MyMpi::isend(_job_tree.getParentNodeRank(), MSG_SCHED_NEW_INACTIVE_NODE, _inactive_job_nodes);
            _inactive_job_nodes.set.clear();
        }
    }
};
