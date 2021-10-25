
#pragma once

#include <vector>
#include <list>
#include <set>

#include "data/serializable.hpp"

struct InactiveJobNode : public Serializable {
    int rank;
    int originalIndex;
    int lastEpoch;
    mutable enum Status {AVAILABLE, BUSY, LOST} status = AVAILABLE;

    InactiveJobNode() {}
    InactiveJobNode(int rank, int index, int epoch) : rank(rank), originalIndex(index), lastEpoch(epoch) {}

    bool operator<(const InactiveJobNode& other) const {
        if (rank != other.rank) return rank < other.rank;
        return lastEpoch > other.lastEpoch;
    }
    bool operator==(const InactiveJobNode& other) const {
        return rank == other.rank && lastEpoch == other.lastEpoch;
    }
    bool operator!=(const InactiveJobNode& other) const {
        return !(*this == other);
    }

    std::vector<uint8_t> serialize() const override {
        std::vector<uint8_t> packed(sizeof(InactiveJobNode));    
        memcpy(packed.data(), this, sizeof(InactiveJobNode));
        return packed;
    }

    Serializable& deserialize(const std::vector<uint8_t>& packed) override {
        memcpy(this, packed.data(), sizeof(InactiveJobNode));
        return *this;
    }
};

struct InactiveJobNodeList : public Serializable {
    std::set<InactiveJobNode> set;
    std::set<InactiveJobNode>::iterator it;

    std::set<InactiveJobNode>::iterator& initIterator() {
        it = set.begin();
        return it;
    }

    bool containsUsableNodes() const {
        for (auto& node : set) {
            if (node.status == InactiveJobNode::AVAILABLE) return true;
        }
        return false;
    }

    void merge(const InactiveJobNodeList& other) {
        for (auto& otherNode : other.set) {
            if (set.count(otherNode)) {
                auto& node = *set.find(otherNode);
                if (node.status == InactiveJobNode::AVAILABLE) {
                    set.erase(otherNode);
                    set.insert(otherNode);
                }
            } else set.insert(otherNode);
        }
    }

    void cleanUp() {
        std::vector<InactiveJobNode> nodesToDelete; 
        for (auto& node : set) {
            if (node.status == InactiveJobNode::LOST)
                nodesToDelete.push_back(node);
            else if (node.status == InactiveJobNode::BUSY)
                node.status = InactiveJobNode::AVAILABLE;
        }
        for (auto& node : nodesToDelete) set.erase(node);
    }

    size_t getTransferSize() const {
        return set.size()*sizeof(InactiveJobNode);
    }

    std::vector<uint8_t> serialize() const override {
        std::vector<uint8_t> packed(getTransferSize());
        int n = sizeof(InactiveJobNode), i = 0;
        for (auto& node : set) {
            memcpy(packed.data()+i, &node, n); i += n;
        }
        return packed;
    }

    Serializable& deserialize(const std::vector<uint8_t>& packed) override {
        int n = sizeof(InactiveJobNode), i = 0;
        while (i < packed.size()) {
            InactiveJobNode node;
            memcpy(&node, packed.data()+i, n); i += n;
            set.insert(node);
        }
        return *this;
    }
};

struct JobSchedulingUpdate : public Serializable {

    int jobId;
    int epoch;
    int volume;
    int difference;
    InactiveJobNodeList inactiveJobNodes;

    JobSchedulingUpdate() {}
    JobSchedulingUpdate(int jobId, int epoch, int volume, int difference, InactiveJobNodeList&& inactiveJobNodes)
        : jobId(jobId), epoch(epoch), volume(volume), difference(difference), inactiveJobNodes(std::move(inactiveJobNodes)) {}

    std::vector<uint8_t> serialize() const override {
        std::vector<uint8_t> packed(4*sizeof(int) + inactiveJobNodes.set.size()*sizeof(InactiveJobNode));
        int i = 0, n = sizeof(int);
        memcpy(packed.data()+i, &jobId, n); i += n;
        memcpy(packed.data()+i, &epoch, n); i += n;
        memcpy(packed.data()+i, &volume, n); i += n;
        memcpy(packed.data()+i, &difference, n); i += n;
        n = sizeof(InactiveJobNode);
        for (auto& node : inactiveJobNodes.set) {
            memcpy(packed.data()+i, &node, n); i += n;
        }
        return packed;
    }

    Serializable& deserialize(const std::vector<uint8_t>& packed) override {
        int i = 0, n = sizeof(int);
        memcpy(&jobId, packed.data()+i, n); i += n;
        memcpy(&epoch, packed.data()+i, n); i += n;
        memcpy(&volume, packed.data()+i, n); i += n;
        memcpy(&difference, packed.data()+i, n); i += n;
        n = sizeof(InactiveJobNode);
        inactiveJobNodes.set.clear();
        while (i < packed.size()) {
            InactiveJobNode node;
            memcpy(&node, packed.data()+i, n); i += n;
            inactiveJobNodes.set.insert(node);
        }
    }
    
    std::pair<JobSchedulingUpdate, JobSchedulingUpdate> split(int index) const {
        std::pair<JobSchedulingUpdate, JobSchedulingUpdate> result;
        auto& [left, right] = result;

        int leftMax = 2*index+1;
        int rightMax = 2*index+2;
        int minChildIdx = leftMax;

        // nodes are sorted in ascending order by rank
        for (auto& node : inactiveJobNodes.set) {
            int index = node.originalIndex;

            if (index < minChildIdx) {
                // just put into smaller of the two partitions
                if (left.inactiveJobNodes.set.size() < right.inactiveJobNodes.set.size())
                    left.inactiveJobNodes.set.insert(node);
                else
                    right.inactiveJobNodes.set.insert(node);
                continue;
            }

            while (index > rightMax) {
                leftMax = 2*leftMax+2; // go down to right child
                rightMax = 2*rightMax+2; // go down to right child
            }

            if (index > leftMax) {
                // right subtree
                right.inactiveJobNodes.set.insert(node);
            } else {
                // left subtree
                left.inactiveJobNodes.set.insert(node);
            }
        }

        return result;
    }
};
