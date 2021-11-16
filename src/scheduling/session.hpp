
#pragma once

class ChildInterface {

private:
    int jobId;
    JobTree& tree;
    InactiveJobNodeList nodes;

    int epoch = -1;
    int volume = -1;
    int childIndex;
    int childRank;

    int numQueriedJobNodes = 0;
    bool notifiedInactiveNodes = false;
    bool childHasNodes = false;

public:
    ChildInterface(int jobId, JobTree& tree, InactiveJobNodeList&& nodes, int childIndex, int childRank) : 
            jobId(jobId), tree(tree), nodes(nodes), childIndex(childIndex), childRank(childRank) {
        log(V5_DEBG, "RBS OPEN_CHILD #%i:%i\n", jobId, childIndex);
    }

    ~ChildInterface() {
        log(V5_DEBG, "RBS CLOSE_CHILD #%i:%i\n", jobId, childIndex);
    }

    // called from local balancer update
    enum MsgDirective {DO_NOTHING, EMIT_DIRECTED_REQUEST, EMIT_UNDIRECTED_REQUEST};
    MsgDirective handleBalancingUpdate(int newEpoch, int newVolume) {
        log(V5_DEBG, "RBS CHILD #%i:%i e=%i->%i v=%i->%i\n", jobId, childIndex, epoch, newEpoch, volume, newVolume);
        if (newEpoch <= epoch) return DO_NOTHING; 
        numQueriedJobNodes = 0;
        
        epoch = newEpoch;
        volume = newVolume;
        
        // Reset availability status of inactive nodes
        for (auto& node : nodes.set) {
            if (node.status == InactiveJobNode::BUSY) {
                // This node was busy in the last epoch; it may be available now.
                node.status = InactiveJobNode::AVAILABLE;
                notifiedInactiveNodes = false;
            }
            if (node.status == InactiveJobNode::AVAILABLE && node.originalIndex >= newVolume) {
                // Node which is not desired for this epoch:
                // No need to notify it later, so mark it busy now.
                node.status = InactiveJobNode::BUSY;
            }
        }

        if (!hasChild() && wantsChild()) {
            // Emit a (new) request with up-to-date epoch.
            return recruitChild();
        }
        return DO_NOTHING;
    }

    // a suspended child returns its inactive job nodes
    void addJobNodesFromSuspendedChild(const InactiveJobNodeList& newNodes) {
        nodes.mergePreferringNewer(newNodes);
        notifiedInactiveNodes = false;
        childHasNodes = false;
    }

    MsgDirective handleRejectionOfPotentialChild(int index, int epoch, bool lost) {
        assert(index == childIndex || log_return_false("ERROR %i != %i\n", index, childIndex));
        findAndUpdateNode(childRank, childIndex, epoch, lost ? InactiveJobNode::LOST : InactiveJobNode::BUSY);
        return recruitChild();
    }

    void handleChildJoining(int source, int index, int epoch) {

        assert(index == childIndex || log_return_false("ERROR %i != %i\n", index, childIndex));
        assert(hasChild());

        childRank = source;
        log(V5_DEBG, "RBS #%i:%i INIT_CHILD e=%i inactives={%s} => [%i]\n", 
            jobId, childIndex, epoch, 
            nodes.toStr().c_str(), source);

        // Transfer the appropriately scoped job nodes for this child
        JobSchedulingUpdate update;
        update.jobId = jobId;
        update.epoch = epoch;
        update.volume = volume;
        update.inactiveJobNodes = nodes.extractSubtree(index, /*excludeRoot=*/true);
        MyMpi::isend(childRank, MSG_SCHED_INITIALIZE_CHILD_WITH_NODES, update);
        childHasNodes = true;

        // If you can find the job node of this rank, set it to CONSUMED.
        findAndUpdateNode(source, index, epoch, InactiveJobNode::CONSUMED);
        notifyRemainingInactiveNodes();
    }

    int getChildIndex() const {
        return childIndex;
    }
    int getChildRank() const {
        return childRank;
    }
    bool doesChildHaveNodes() const {
        return childHasNodes;
    }

    InactiveJobNodeList&& returnJobNodes() {
        notifyRemainingInactiveNodes();
        return std::move(nodes);
    }

    bool hasChild() const {
        return (tree.getLeftChildIndex() == childIndex) ? tree.hasLeftChild() : tree.hasRightChild();
    }
    bool wantsChild() const {
        return ((tree.getLeftChildIndex() == childIndex) ? tree.getLeftChildIndex() : tree.getRightChildIndex()) < volume;
    }

    int getEpoch() const {
        return epoch;
    }

private:
    /* --- helpers --- */
    MsgDirective recruitChild() {
        
        auto it = nodes.set.begin();
        for (; numQueriedJobNodes < 4 && it != nodes.set.end(); it++) {
            auto& node = *it;
            if (node.status != InactiveJobNode::AVAILABLE) {
                // Node is known to be lost or busy: do not query
                continue;
            }
            // Query node for adoption
            childRank = node.rank;
            tree.updateJobNode(childIndex, childRank);
            numQueriedJobNodes++;
            return EMIT_DIRECTED_REQUEST; // wait for response 
        }

        notifyRemainingInactiveNodes();
        return EMIT_UNDIRECTED_REQUEST;
    }

    void notifyRemainingInactiveNodes() {
        if (notifiedInactiveNodes) return;
        for (auto& node : nodes.set) {
            if (node.status == InactiveJobNode::AVAILABLE) {
                MyMpi::isend(node.rank, MSG_SCHED_RELEASE_FROM_WAITING, IntPair(jobId, epoch));
            }
        }
        notifiedInactiveNodes = true;
    }

    void findAndUpdateNode(int rank, int index, int epoch, InactiveJobNode::Status status) {
        
        InactiveJobNode node(rank, index, INT32_MAX);
        bool found = false;
        
        auto it = nodes.set.lower_bound(node);
        for (; it != nodes.set.end(); ++it) {
            auto& n = *it;
            if (n.originalIndex != index) return;
            if (n.rank != rank) continue;
            
            // Correct node found
            node = n;
            nodes.set.erase(n);

            node.lastEpoch = epoch;
            node.status = status;
            nodes.set.insert(node);
            log(V5_DEBG, "RBS Update (%i,%i,%i) -> status %i\n", rank, index, epoch, status);
            return;
        }
    }
};
