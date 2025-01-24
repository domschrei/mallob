
#pragma once

#include <vector>

#include "comm/job_tree_snapshot.hpp"
#include "data/job_transfer.hpp"
#include "util/assert.hpp"

class GroupComm : public Serializable {

private:
    std::vector<int> _ranks;
    std::vector<ctx_id_t> _ctx_ids;

    ctx_id_t _my_ctx_id;
    int _my_local_rank {-1};

public:
    GroupComm() {}
    GroupComm(const std::vector<std::pair<int, ctx_id_t>>& addresses, ctx_id_t myContextId) : _my_ctx_id(myContextId) {
        for (auto [rank, ctxId] : addresses) {
            if (ctxId == _my_ctx_id) _my_local_rank = _ranks.size();
            _ranks.push_back(rank);
            _ctx_ids.push_back(ctxId);
        }
    }

    int getMyLocalRank() const {
        return _my_local_rank;
    }

    int getWorldRank(int localRank) const {
        assert(localRank >= 0);
        assert(localRank < _ranks.size());
        return _ranks.at(localRank);
    }

    ctx_id_t getContextId(int localRank) const {
        assert(localRank >= 0);
        assert(localRank < _ranks.size());
        return _ctx_ids.at(localRank);
    }

    int getCommSize() const {
        return _ranks.size();
    }

    void localize(ctx_id_t myContextId) {
        _my_ctx_id = myContextId;
        for (size_t i = 0; i < _ctx_ids.size(); i++) {
            if (_ctx_ids[i] == _my_ctx_id) {
                _my_local_rank = i;
                return;
            }
        }
        assert(false);
    }

    JobTreeSnapshot getTreeSnapshot() const {
        JobTreeSnapshot snapshot;
        snapshot.nodeRank = getWorldRank(_my_local_rank);
        snapshot.index = _my_local_rank;
        snapshot.contextId = _my_ctx_id;

        const int leftChildIdx = 2*_my_local_rank + 1;
        const int rightChildIdx = 2*_my_local_rank + 2;
        const int parentIdx = (_my_local_rank-1) / 2;

        const bool hasLeftChild = leftChildIdx < getCommSize();
        const bool hasRightChild = rightChildIdx < getCommSize();
        snapshot.nbChildren = (hasLeftChild?1:0) + (hasRightChild?1:0);
        
        snapshot.leftChildIndex = hasLeftChild ? leftChildIdx : -1;
        snapshot.leftChildNodeRank = hasLeftChild ? getWorldRank(leftChildIdx) : -1;
        snapshot.leftChildContextId = hasLeftChild ? getContextId(leftChildIdx) : 0;
        snapshot.rightChildIndex = hasRightChild ? rightChildIdx : -1;
        snapshot.rightChildNodeRank = hasRightChild ? getWorldRank(rightChildIdx) : -1;
        snapshot.rightChildContextId = hasRightChild ? getContextId(rightChildIdx) : 0;
        snapshot.parentIndex = parentIdx;
        snapshot.parentNodeRank = getWorldRank(parentIdx);
        snapshot.parentContextId = getContextId(parentIdx);

        return snapshot;
    }

    std::string toStr() const {
        std::string summary;
        for (size_t i = 0; i < _ranks.size(); i++) {
            summary += std::to_string(_ranks[i]) + "," + std::to_string(_ctx_ids[i]) + " ";
        }
        return summary;
    }

    virtual std::vector<uint8_t> serialize() const override {
        std::vector<uint8_t> packed(_ranks.size() * (sizeof(int) + sizeof(ctx_id_t)));
        memcpy(packed.data(), _ranks.data(), _ranks.size() * sizeof(int));
        memcpy(packed.data() + _ranks.size() * sizeof(int), _ctx_ids.data(), _ctx_ids.size() * sizeof(ctx_id_t));
        return packed;
    }
    virtual GroupComm& deserialize(const std::vector<uint8_t>& packed) override {
        int commSize = packed.size() / (sizeof(int)+sizeof(ctx_id_t));
        _ranks.resize(commSize);
        memcpy(_ranks.data(), packed.data(), _ranks.size() * sizeof(int));
        _ctx_ids.resize(commSize);
        memcpy(_ctx_ids.data(), packed.data() + _ranks.size() * sizeof(int), _ctx_ids.size() * sizeof(ctx_id_t));
        return *this;
    }
};
