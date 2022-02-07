
#pragma once

class ChildInterface {

private:
    int jobId;
    InactiveJobNodeList nodes;

    int epoch = -1;
    int volume = -1;
    int childIndex;
    int childRank = -1;

    int numQueriedJobNodes = 0;
    bool childHasNodes = false;

public:
    ChildInterface(int jobId, InactiveJobNodeList&& nodes, int childIndex) : 
            jobId(jobId), nodes(nodes), childIndex(childIndex) {
        LOG(V5_DEBG, "RBS OPEN_CHILD #%i:%i inactives={%s}\n", jobId, childIndex, nodes.toStr().c_str());
    }

    ~ChildInterface() {
        LOG(V5_DEBG, "RBS CLOSE_CHILD #%i:%i\n", jobId, childIndex);
    }

    // called from local balancer update
    enum MsgDirective {DO_NOTHING, EMIT_DIRECTED_REQUEST, EMIT_UNDIRECTED_REQUEST};
    MsgDirective handleBalancingUpdate(int newEpoch, int newVolume, bool hasChild) {
        LOG(V5_DEBG, "RBS CHILD #%i:%i e=%i->%i v=%i->%i\n", jobId, childIndex, epoch, newEpoch, volume, newVolume);
        if (newEpoch <= epoch) return DO_NOTHING; 
        numQueriedJobNodes = 0;
        
        epoch = newEpoch;
        volume = newVolume;
        
        // Reset availability status of inactive nodes
        for (auto& node : nodes.set) {
            if (node.status == InactiveJobNode::BUSY) {
                // This node was busy in the last epoch; it may be available now.
                node.status = InactiveJobNode::AVAILABLE;
            }
            if (node.status == InactiveJobNode::AVAILABLE && node.originalIndex >= newVolume) {
                // Node which is not desired for this epoch:
                // No need to notify it later, so mark it busy now.
                node.status = InactiveJobNode::BUSY;
            }
        }

        if (!hasChild && wantsChild()) {
            // Emit a (new) request with up-to-date epoch.
            return recruitChild();
        } else {
            notifyRemainingInactiveNodes();
            return DO_NOTHING;
        }
    }

    // a suspended child returns its inactive job nodes
    void addJobNodesFromSuspendedChild(int rank, InactiveJobNodeList& newNodes) {
        nodes.mergePreferringNewer(newNodes);
        //notifiedInactiveNodes = false;
        childHasNodes = false;
        LOG(V4_VVER, "RBS #%i:%i SHRINK #inactives=%i <= [%i]\n", jobId, childIndex, nodes.set.size(), rank);
    }

    MsgDirective handleRejectionOfPotentialChild(int index, int epoch, bool lost, bool hasChild, bool suspended) {
        assert(index == childIndex || LOG_RETURN_FALSE("ERROR %i != %i\n", index, childIndex));
        findAndUpdateNode(childRank, childIndex, epoch, lost ? InactiveJobNode::LOST : InactiveJobNode::BUSY);
        if (!hasChild && !suspended && wantsChild()) return recruitChild();
        return DO_NOTHING;
    }

    void handleChildJoining(int source, int index, int epoch) {

        assert(index == childIndex || LOG_RETURN_FALSE("ERROR %i != %i\n", index, childIndex));

        childRank = source;
        childHasNodes = true;

        // Transfer the appropriately scoped job nodes for this child
        JobSchedulingUpdate update;
        update.jobId = jobId;
        update.destinationIndex = childIndex;
        update.epoch = epoch;
        update.volume = volume;
        update.inactiveJobNodes = nodes.extractSubtree(index, /*excludeRoot=*/true);
        update.inactiveJobNodes.cleanUpStatuses();
        MyMpi::isend(childRank, MSG_SCHED_INITIALIZE_CHILD_WITH_NODES, update);

        LOG(V4_VVER, "RBS #%i:%i EXPAND e=%i #inactives=%i => [%i]\n", 
            jobId, childIndex, epoch, update.inactiveJobNodes.set.size(), source);

        // If you can find the job node of this rank, set it to BUSY.
        findAndUpdateNode(source, index, epoch, InactiveJobNode::BUSY);
        // Any left job nodes which are still available must be released from waiting
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
        LOG(V5_DEBG, "RBS #%i:%i RETURNING_NODES inactives={%s}\n", jobId, childIndex, nodes.toStr().c_str());
        return std::move(nodes);
    }

    bool wantsChild() const {
        return childIndex < volume;
    }

    int getEpoch() const {
        return epoch;
    }

    void removeNode(int rank, int epoch, int index) {
        InactiveJobNode node(rank, index, epoch);
        if (nodes.set.count(node)) {
            LOG(V5_DEBG, "RBS #%i:%i DELETE (%i,%i,%i)\n", 
                jobId, childIndex, rank, index, epoch);
            nodes.set.erase(node);
        }
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
            if (node.originalIndex != childIndex) {
                // Node does not have the correct index to become this node's child
                continue;
            }
            // Query node for adoption
            childRank = node.rank;
            //tree.updateJobNode(childIndex, childRank);
            numQueriedJobNodes++;
            return EMIT_DIRECTED_REQUEST; // wait for response 
        }

        // No success: Notify remaining nodes and emit an undirected request
        notifyRemainingInactiveNodes(childIndex);
        return EMIT_UNDIRECTED_REQUEST;
    }

    void notifyRemainingInactiveNodes(int particularIndex = -1) {
        for (auto& node : nodes.set) {
            if (node.status == InactiveJobNode::AVAILABLE 
                    && (particularIndex == -1 || node.originalIndex == particularIndex)) {
                LOG(V5_DEBG, "RBS #%i:%i e=%i RELEASE (%i,%i,%i,%s)\n", jobId, childIndex, epoch,
                        node.rank, node.originalIndex, node.lastEpoch, InactiveJobNode::STATUS_STR[node.status]);
                MyMpi::isend(node.rank, MSG_SCHED_RELEASE_FROM_WAITING, IntVec({jobId, node.originalIndex, epoch}));
                node.status = InactiveJobNode::BUSY;
            }
        }
    }

    bool findAndUpdateNode(int rank, int index, int epoch, InactiveJobNode::Status status) {
        
        InactiveJobNode node(rank, index, INT32_MAX);        
        auto it = nodes.set.lower_bound(node);

        for (; it != nodes.set.end(); ++it) {
            auto& n = *it;
            if (n.originalIndex != index) break;
            if (n.rank != rank) continue;

            // Correct node found
            node = n;
            nodes.set.erase(n);

            node.lastEpoch = epoch;
            node.status = status;
            nodes.set.insert(node);
            LOG(V5_DEBG, "RBS #%i:%i Update (%i,%i,%i) -> status %i\n", jobId, childIndex, rank, index, epoch, status);
            return true;
        }

        LOG(V5_DEBG, "RBS #%i:%i (%i,%i,%i) not found\n", jobId, childIndex, rank, index, epoch);
        return false;
    }
};
