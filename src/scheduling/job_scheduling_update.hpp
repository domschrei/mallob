
#pragma once

#include <vector>
#include <list>
#include <set>
#include <string>

#include "util/hashing.hpp"
#include "data/serializable.hpp"
#include "util/assert.hpp"

struct InactiveJobNode : public Serializable {
    int rank;
    int originalIndex;
    int lastEpoch;
    mutable enum Status {AVAILABLE, BUSY, LOST} status = AVAILABLE;
    static const char* STATUS_STR[4];

    InactiveJobNode() {}
    InactiveJobNode(int rank, int index, int epoch) : rank(rank), originalIndex(index), lastEpoch(epoch) {}

    bool operator<(const InactiveJobNode& other) const {
        if (originalIndex != other.originalIndex) return originalIndex < other.originalIndex;
        //if (rank != other.rank) return rank < other.rank;
        if (lastEpoch != other.lastEpoch) return lastEpoch > other.lastEpoch;
        return rank < other.rank;
    }
    bool operator==(const InactiveJobNode& other) const {
        return originalIndex == other.originalIndex && rank == other.rank && lastEpoch == other.lastEpoch;
    }
    bool operator!=(const InactiveJobNode& other) const {
        return !(*this == other);
    }

    std::vector<uint8_t> serialize() const override {
        std::vector<uint8_t> packed(sizeof(InactiveJobNode));    
        memcpy(packed.data(), this, sizeof(InactiveJobNode));
        return packed;
    }

    InactiveJobNode& deserialize(const std::vector<uint8_t>& packed) override {
        memcpy(this, packed.data(), sizeof(InactiveJobNode));
        return *this;
    }
};

typedef std::set<InactiveJobNode>::iterator InactiveJobNodeIterator;

struct InactiveJobNodeList : public Serializable {
    std::set<InactiveJobNode> set;

    bool containsUsableNodes() const {
        for (auto& node : set) {
            if (node.status == InactiveJobNode::AVAILABLE) return true;
        }
        return false;
    }

    void mergePreferringNewer(const InactiveJobNodeList& other) {
        set.insert(other.set.begin(), other.set.end());
        filterDuplicates();
    }

    void mergeReplacing(const InactiveJobNodeList& other) {
        
        robin_hood::unordered_map<int, InactiveJobNode> nodesByRank;
        for (auto& node : set) nodesByRank[node.rank] = node;

        for (auto& otherNode : other.set) {
            if (nodesByRank.count(otherNode.rank))
                set.erase(nodesByRank.at(otherNode.rank));
            set.insert(otherNode);
        }
    }

    InactiveJobNodeList extractSubtree(int indexOfRoot, bool excludeRoot) {

        int leftIndex = indexOfRoot;
        int rightIndex = indexOfRoot;
        if (excludeRoot) {
            leftIndex = 2*leftIndex+1;
            rightIndex = 2*rightIndex+2;
        }
        
        InactiveJobNodeList result;

        for (auto& node : set) {
            while (node.originalIndex > rightIndex) {
                // Go to next layer of tree
                leftIndex = 2*leftIndex+1; // left child of leftmost node
                rightIndex = 2*rightIndex+2; // right child of rightmost node
            }
            if (node.originalIndex >= leftIndex && node.originalIndex <= rightIndex) {
                result.set.insert(node);
            }
        }

        for (auto& node : result.set) set.erase(node);

        return result;
    }

    void cleanUpStatuses() {

        std::vector<InactiveJobNode> nodesToDelete; // track nodes to remove
        
        for (auto& node : set) {
            if (node.status == InactiveJobNode::LOST)
                nodesToDelete.push_back(node);
            else node.status = InactiveJobNode::AVAILABLE; // reset temporarily busy nodes to available
        }

        for (auto& node : nodesToDelete) set.erase(node);
    }

    void filterDuplicates() {
        
        std::vector<InactiveJobNode> nodesToDelete; // track nodes to remove
        robin_hood::unordered_map<int, InactiveJobNode> nodesByRank; // track possible redundancies

        for (auto& node : set) {

            // Already seen this rank?
            if (nodesByRank.count(node.rank)) {
                auto& otherNode = nodesByRank.at(node.rank);
                if (node.lastEpoch > otherNode.lastEpoch) {
                    // later epoch: keep this node, remove the contained node
                    nodesToDelete.push_back(otherNode);
                    nodesByRank[node.rank] = node;
                } else {
                    // keep the contained node, skip this node
                    nodesToDelete.push_back(node);
                    continue;
                }
            } else {
                // -- no: remember
                nodesByRank[node.rank] = node;
            }
        }

        // remove unneeded / excess nodes
        for (auto& node : nodesToDelete) set.erase(node);
    }

    size_t getTransferSize() const {
        return set.size()*sizeof(InactiveJobNode);
    }

    std::vector<uint8_t> serialize() const override {
        std::vector<uint8_t> packed(sizeof(int)+getTransferSize());
        int size = set.size();
        memcpy(packed.data(), &size, sizeof(int));
        int n = sizeof(InactiveJobNode), i = sizeof(int);
        for (auto& node : set) {
            memcpy(packed.data()+i, &node, n); i += n;
        }
        assert(i == packed.size());
        return packed;
    }

    InactiveJobNodeList& deserialize(const std::vector<uint8_t>& packed) override {
        int n = sizeof(int), i = 0;
        int size;
        memcpy(&size, packed.data()+i, n); i += n;
        n = sizeof(InactiveJobNode);
        for (size_t j = 0; j < size; j++) {
            InactiveJobNode node;
            memcpy(&node, packed.data()+i, n); i += n;
            set.insert(node);
        }
        assert(size == set.size());
        assert(i == packed.size());
        return *this;
    }

    std::string toStr() const {
        std::string out = "";
        for (auto& node : set) {
            out += "(" + std::to_string(node.rank) + "," + std::to_string(node.originalIndex) + "," 
                + std::to_string(node.lastEpoch) + "," + std::string(InactiveJobNode::STATUS_STR[node.status]) + "), ";
        }
        return out.empty() ? out : out.substr(0, out.size()-2);
    }
};

struct JobSchedulingUpdate : public Serializable {

    int jobId = -1;
    int destinationIndex = -1;
    int epoch = -1;
    int volume = -1;
    InactiveJobNodeList inactiveJobNodes;

    JobSchedulingUpdate() {}
    JobSchedulingUpdate(int jobId, int epoch, int volume, InactiveJobNodeList&& inactiveJobNodes)
        : jobId(jobId), epoch(epoch), volume(volume), inactiveJobNodes(std::move(inactiveJobNodes)) {}

    std::vector<uint8_t> serialize() const override {
        std::vector<uint8_t> packed(4*sizeof(int) + inactiveJobNodes.set.size()*sizeof(InactiveJobNode));
        int i = 0, n = sizeof(int);
        memcpy(packed.data()+i, &jobId, n); i += n;
        memcpy(packed.data()+i, &destinationIndex, n); i += n;
        memcpy(packed.data()+i, &epoch, n); i += n;
        memcpy(packed.data()+i, &volume, n); i += n;
        n = sizeof(InactiveJobNode);
        for (auto& node : inactiveJobNodes.set) {
            memcpy(packed.data()+i, &node, n); i += n;
        }
        return packed;
    }

    Serializable& deserialize(const std::vector<uint8_t>& packed) override {
        int i = 0, n = sizeof(int);
        memcpy(&jobId, packed.data()+i, n); i += n;
        memcpy(&destinationIndex, packed.data()+i, n); i += n;
        memcpy(&epoch, packed.data()+i, n); i += n;
        memcpy(&volume, packed.data()+i, n); i += n;
        n = sizeof(InactiveJobNode);
        inactiveJobNodes.set.clear();
        while (i < packed.size()) {
            InactiveJobNode node;
            memcpy(&node, packed.data()+i, n); i += n;
            inactiveJobNodes.set.insert(node);
        }
        return *this;
    }
    
    std::pair<JobSchedulingUpdate, JobSchedulingUpdate> split(int index) const {
        std::pair<JobSchedulingUpdate, JobSchedulingUpdate> result;
        auto& [left, right] = result;
        
        left.jobId = right.jobId = jobId;
        left.epoch = right.epoch = epoch;
        left.volume = right.volume = volume;

        for (auto& node : inactiveJobNodes.set) {
            int nodeIndex = node.originalIndex;
            int leftMax = 2*index+1;
            int rightMax = 2*index+2;
            int minChildIdx = leftMax;

            if (nodeIndex < minChildIdx) {
                // just put into smaller of the two partitions
                if (left.inactiveJobNodes.set.size() < right.inactiveJobNodes.set.size())
                    left.inactiveJobNodes.set.insert(node);
                else
                    right.inactiveJobNodes.set.insert(node);
                continue;
            }

            while (nodeIndex > rightMax) {
                leftMax = 2*leftMax+2; // go down to right child
                rightMax = 2*rightMax+2; // go down to right child
            }

            if (nodeIndex > leftMax) {
                // right subtree
                right.inactiveJobNodes.set.insert(node);
            } else {
                // left subtree
                left.inactiveJobNodes.set.insert(node);
            }
        }

        return result;
    }

    bool valid() const {return jobId >= 0;}
};
