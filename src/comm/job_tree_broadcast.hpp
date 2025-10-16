
#pragma once

#include "app/job_tree.hpp"
#include "comm/job_tree_snapshot.hpp"
#include "comm/msg_queue/cond_message_subscription.hpp"
#include "comm/msg_queue/message_handle.hpp"
#include "comm/msgtags.h"
#include "data/job_transfer.hpp"

class JobTreeBroadcast {

private:
    const int _job_id;
    JobTreeSnapshot _tree;
    int _internal_msg_tag;
    CondMessageSubscription _sub_broadcast;

    bool _received_broadcast {false};
    JobMessage _msg;
    bool _received_response_left {false};
    bool _received_response_right {false};
    bool _result_extracted {false};

    //The default callback, to be set when Object is initialized
    std::function<void()> _cb;

public:
    // jobId : Job ID this broadcast is associated with
    // tree : JobTree instance of the associated worker
    // internalMsgTag: positive integer that must be consistent across all participating workers,
    //   or alternatively -1 (default) to match *any* broadcast going on in this job.
    JobTreeBroadcast(int jobId, const JobTreeSnapshot& tree, std::function<void()> callback = []() {}, int internalMsgTag = -1) :
        _job_id(jobId), _tree(tree), _internal_msg_tag(internalMsgTag),
        _sub_broadcast(MSG_JOB_TREE_MODULAR_BROADCAST, [&](MessageHandle& h) {return receiveMessage(h);}) {
        _cb = callback;
        //The default callback is digestBroadcast()
    }

    void broadcast(JobMessage&& msg, bool rootOfBcast = true) {

        assert(!_msg.returnedToSender); // valid message, not returned undeliverable
        _msg = std::move(msg);
        assert(_msg.jobId == _job_id);
        assert(_internal_msg_tag == -1 || _msg.tag == _internal_msg_tag);
        _internal_msg_tag = _msg.tag;

        LOG(V4_VVER, "BCAST in broadcast(). isRoot? %i _received_broadcast? %i \n", rootOfBcast, _received_broadcast);
        _received_broadcast = true;

        assert(!_msg.returnedToSender);
        _tree.sendToAnyChildren(_msg, MSG_JOB_TREE_MODULAR_BROADCAST);

        if (_tree.leftChildNodeRank < 0) _received_response_left = true;
        if (_tree.rightChildNodeRank < 0) _received_response_right = true;

        if (!rootOfBcast) {
            // Create response message without copying the original message's payload
            auto payload = std::move(_msg.payload);
            JobMessage response(_msg);
            _msg.payload = std::move(payload);

            _tree.sendToParent(_msg, MSG_JOB_TREE_MODULAR_BROADCAST);
        }

        if (hasResult()) _cb(); // execute the provided callback. In case of SweepJob it is SweepJob::cbContributeToAllReduce()
    }

    void updateJobTree(const JobTree& tree) {
        if (_received_broadcast) return;
        _tree = tree.getSnapshot();
    }

    bool hasResult() {
        return _received_broadcast && _received_response_left && _received_response_right && !_result_extracted;
    }
    JobMessage&& getJobMessage() {
        assert(hasResult());
        _result_extracted = true;
        return std::move(_msg);
    }
    const JobTreeSnapshot& getJobTreeSnapshot() {
        return _tree;
    }

    int getMessageTag() const {
        return _internal_msg_tag;
    }

    bool getReceivedBroadcast() {
        return _received_broadcast;
    }

private:
    bool receiveMessage(MessageHandle& h) {
        //received on MSG_JOB_TREE_MODULAR_BROADCAST mpiTag (via _sub_broadcast(...))
        JobMessage msg = Serializable::get<JobMessage>(h.getRecvData());

        LOG(V4_VVER, "BCAST received msg from sourceRank %i\n", h.source);
        // Right recipient?
        if (msg.jobId != _job_id) return false;
        if (_internal_msg_tag >= 0 && msg.tag != _internal_msg_tag) return false;

        // Undeliverable message being returned?
        if (msg.returnedToSender) {
            // prune child
            LOG(V4_VVER, "BCAST returnToSender received from sourceRank %i\n", h.source);
            if (h.source == _tree.leftChildNodeRank) {
                _tree.leftChildNodeRank = -1;
                _received_response_left = true;
            }
            if (h.source == _tree.rightChildNodeRank) {
                _tree.rightChildNodeRank = -1;
                _received_response_right = true;
            }
            if (hasResult()) _cb(); // callback
            return true;
        }

        LOG(V4_VVER, "BCAST received msg from sourceRank %i (local _received_broadcast=%i, leftChildRank %i, RightChildRank %i)\n",
            h.source, _received_broadcast, _tree.leftChildNodeRank, _tree.rightChildNodeRank);

        // Response from child?
        if (_received_broadcast && h.source == _tree.leftChildNodeRank) {
            _received_response_left = true;
            if (hasResult()) _cb(); // callback
            return true;
        }
        if (_received_broadcast && h.source == _tree.rightChildNodeRank) {
            _received_response_right = true;
            if (hasResult()) _cb(); // callback
            return true;
        }

        // Advance broadcast

        LOG(V4_VVER, "BCAST received message, but source doesnt match any children or we not yet self received, advance \n");
        broadcast(std::move(msg), false);
        return true;
    }
};
