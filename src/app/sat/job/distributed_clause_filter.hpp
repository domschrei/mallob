
#pragma once

#include <stdint.h>

#include "../data/clause.hpp"
#include "../sharing/filter/clause_filter.hpp"
#include "app/job_tree.hpp"

class DistributedClauseFilter {

private:
    robin_hood::unordered_flat_map<Mallob::Clause, int, NonCommutativeClauseHasher, SortedClauseExactEquals> _filter;

    int _last_index = -1;
    int _last_volume = -1;

    int _max_slot_index;
    int _my_slot_index;
    int _left_slot_index;
    int _right_slot_index;

    int _num_remembered_epochs;

public:
    DistributedClauseFilter(int numRememberedEpochs) : 
        _num_remembered_epochs(numRememberedEpochs >= 0 ? numRememberedEpochs : INT32_MAX) {}

    void update(int index, int volume) {
        if (index == _last_index && volume == _last_volume) return;
        _last_index = index;
        _last_volume = volume;

        _my_slot_index = getSlotIndex(index, volume);

        // If this node has a (left|right) child, this is its direct in-order neighbor
        _left_slot_index = 2*index+1 < volume ? _my_slot_index-1 : -1;
        _right_slot_index = 2*index+2 < volume ? _my_slot_index+1 : -1;

        _left_slot_index = -1;
        _right_slot_index = -1;

        // Go down to (right|left)most child in the (left|right) subtree
        for (int childVal = 1; childVal <= 2; childVal++) {
            int& neighborIndex = childVal == 1 ? _left_slot_index : _right_slot_index;
            int childIndex = 2*index+childVal;
            while (childIndex < volume) {
                neighborIndex = childIndex;    
                childIndex = 2*childIndex+(3-childVal);
            }
            // Conversion from tree index to slot index
            if (neighborIndex != -1) neighborIndex = getSlotIndex(neighborIndex, volume);
        }

        // Only node? -> no parents.
        if (index == 0) return;

        // Go upwards until finding a (smaller|larger) slot index than your own one
        int parentIndex = index;
        while (_left_slot_index == -1 || _right_slot_index == -1) {
            parentIndex = (parentIndex-1) / 2;
            int parentSlot = getSlotIndex(parentIndex, volume);
            if (_left_slot_index == -1 && parentSlot < _my_slot_index)
                _left_slot_index = parentSlot;
            if (_right_slot_index == -1 && parentSlot > _my_slot_index)
                _right_slot_index = parentSlot;
            if (parentIndex == 0) break;
        }
    }

    bool passClause(Mallob::Clause& clause, int epoch) {

        size_t hash = ClauseHasher::hash(clause, /*which=*/3);

        auto it = _filter.find(clause);
        bool contained = it != _filter.end();

        if (contained && epoch - it->second > _num_remembered_epochs) {
            // Obsolete epoch! Remove entry and pretend it was never there
            contained = false;
            free(it->first.begin);
            _filter.erase(it);
        }

        // Already contained: clause does not pass.
        if (contained) return false;

        // Am I responsible for this clause?
        if (isResponsibleFor(hash) && !contained) {
            Clause copy = clause;
            copy.begin = (int*) malloc(sizeof(int)*clause.size);
            memcpy(copy.begin, clause.begin, sizeof(int)*clause.size);
            _filter[copy] = epoch; // register
        }

        return true; // no duplicate was found: success
    }

    ~DistributedClauseFilter() {
        for (auto& [c, val] : _filter) free(c.begin);
    }

    bool isResponsibleFor(size_t hash) {
        double hashPivot = (((double) hash) / SIZE_MAX) * (_max_slot_index);
        double myPivot = _my_slot_index - 0.5;
        //LOG(V2_INFO, "%.5f %.5f\n", hashPivot, myPivot);
        if (_left_slot_index == -1 && hashPivot <= myPivot) return true;
        if (_right_slot_index == -1 && hashPivot >= myPivot) return true;
        double leftPivot = _left_slot_index - 0.5;
        double rightPivot = _right_slot_index - 0.5;
        return std::abs(hashPivot-myPivot) <= std::abs(hashPivot-leftPivot)
            && std::abs(hashPivot-myPivot) <= std::abs(hashPivot-rightPivot);
    }

    std::vector<int> getSlotIndices() const {
        std::vector<int> indices;
        indices.push_back(_left_slot_index);
        indices.push_back(_my_slot_index);
        indices.push_back(_right_slot_index);
        return indices;
    }

    size_t size() const {
        return _filter.size();
    }

private:
    int getSlotIndex(int treeIndex, int volume) {

        int powerOfTwoVolume = 1;
        while (powerOfTwoVolume <= volume) powerOfTwoVolume <<= 1;
        _max_slot_index = powerOfTwoVolume-1;

        int i = treeIndex+1;
        int slotIndex = powerOfTwoVolume/2;
        if (i > 1) {
            // Get largest power of two which is smaller than the MSB of i
            int iPower = 1; while (4*iPower <= i) iPower *= 2;
            //LOG(V2_INFO, "%i : %i\n", i, iPower);
            int offset = slotIndex;
            while (iPower > 0) {
                offset /= 2;
                if ((i & iPower) == 0) {
                    // Go to left child
                    //LOG(V2_INFO, "%i LEFT\n", i&iPower);
                    slotIndex -= offset;
                } else {
                    // Go to right child
                    //LOG(V2_INFO, "%i RIGHT\n", i&iPower);
                    slotIndex += offset;
                }
                iPower /= 2;
            }
        }
        
        //LOG(V2_INFO, "%i : slot index %i\n", treeIndex, slotIndex);
        assert(slotIndex >= 0);
        assert(slotIndex <= _max_slot_index);
        return slotIndex;
    }

};
