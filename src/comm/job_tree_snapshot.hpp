
#pragma once

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
};
