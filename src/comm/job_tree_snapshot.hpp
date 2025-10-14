
#pragma once

#include "comm/mympi.hpp"
#include "data/job_transfer.hpp"
struct JobTreeSnapshot {
    int nodeRank;
    int index;
    int contextId;
    int nbChildren;
    int leftChildNodeRank;
    int leftChildIndex;
    int leftChildContextId;
    int rightChildNodeRank;
    int rightChildIndex;
    int rightChildContextId;
    int parentNodeRank;
    int parentIndex;
    int parentContextId;

    void sendToParent(JobMessage& msg, int mpiTag) const {
        msg.contextIdOfDestination = parentContextId;
        msg.treeIndexOfDestination = parentIndex;
        send(parentNodeRank, mpiTag, msg);
    }
    void sendToLeftChild(JobMessage& msg, int mpiTag) const {
        msg.contextIdOfDestination = leftChildContextId;
        msg.treeIndexOfDestination = leftChildIndex;
        // assert(msg.contextIdOfDestination != 0
            // || log_return_false("Error in Brodcast! Want to send to left child but its contextId is 0! mpiTag %i, msgTag %i, senderIdx %i, destinationIdx %i \n",
                // mpiTag, msg.tag, msg.treeIndexOfSender, msg.treeIndexOfDestination));
        send(leftChildNodeRank, mpiTag, msg);
    }
    void sendToRightChild(JobMessage& msg, int mpiTag) const {
        msg.contextIdOfDestination = rightChildContextId;
        msg.treeIndexOfDestination = rightChildIndex;
        // assert(msg.contextIdOfDestination != 0
            // || log_return_false("Error in Brodcast! Want to send to right child but its contextId is 0! mpiTag %i, msgTag %i, senderIdx %i, destinationIdx %i \n",
                // mpiTag, msg.tag, msg.treeIndexOfSender, msg.treeIndexOfDestination));
        send(rightChildNodeRank, mpiTag, msg);
    }
    void sendToAnyChildren(JobMessage& msg, int mpiTag) const {
        if (leftChildNodeRank >= 0) sendToLeftChild(msg, mpiTag);
        if (rightChildNodeRank >= 0) sendToRightChild(msg, mpiTag);
    }
    void send(int dest, int mpiTag, JobMessage& msg) const {
        msg.contextIdOfSender = contextId;
        msg.treeIndexOfSender = index;
        assert(msg.contextIdOfDestination != 0
            || log_return_false("Error in Brodcast! Want to send within tree but destination contextId is 0! "
                                "mpiTag %i, msgTag %i, senderIdx %i, destinationIdx %i , senderContextId %i, destRank %i, "
                                "leftChildRank %i rightChildRank %i parentRank %i \n",
                mpiTag, msg.tag, msg.treeIndexOfSender, msg.treeIndexOfDestination, contextId, dest, leftChildNodeRank, rightChildNodeRank, parentNodeRank));
        MyMpi::isend(dest, mpiTag, msg);
    }
};
