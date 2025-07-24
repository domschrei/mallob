
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
        send(leftChildNodeRank, mpiTag, msg);
    }
    void sendToRightChild(JobMessage& msg, int mpiTag) const {
        msg.contextIdOfDestination = rightChildContextId;
        msg.treeIndexOfDestination = rightChildIndex;
        send(rightChildNodeRank, mpiTag, msg);
    }
    void sendToAnyChildren(JobMessage& msg, int mpiTag) const {
        if (leftChildNodeRank >= 0) sendToLeftChild(msg, mpiTag);
        if (rightChildNodeRank >= 0) sendToRightChild(msg, mpiTag);
    }
    void send(int dest, int mpiTag, JobMessage& msg) const {
        msg.contextIdOfSender = contextId;
        msg.treeIndexOfSender = index;
        MyMpi::isend(dest, mpiTag, msg);
    }
};
