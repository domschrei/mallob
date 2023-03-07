
#pragma once

#include <atomic>
#include <list>
#include <limits>
#include <memory>
#include <numeric>


#include "../../data/produced_clause.hpp"
#include "bucket_label.hpp"
#include "buffer_reader.hpp"
#include "buffer_merger.hpp"
#include "util/logger.hpp"
#include "util/periodic_event.hpp"
#include "../../data/solver_statistics.hpp"
#include "clause_slot.hpp"

class PriorityClauseBuffer {

private:
    int _total_literal_limit;
    std::vector<std::unique_ptr<ClauseSlot>> _slots;
    std::atomic_int _free_budget {0};
    std::atomic_int _max_admissible_cls_size {std::numeric_limits<int>::max()};

    enum ClauseSlotMode {SAME_SUM_OF_SIZE_AND_LBD, SAME_SIZE, SAME_SIZE_AND_LBD};
    robin_hood::unordered_flat_map<std::pair<int, int>, std::pair<int, ClauseSlotMode>, IntPairHasher> _size_lbd_to_slot_idx_mode;

    int _max_lbd_partitioned_size;
    int _max_clause_length;
    bool _slots_for_sum_of_length_and_lbd;

    bool _use_checksum;
    BucketLabel _bucket_iterator;

    ClauseHistogram _hist_deleted_in_slots;

    bool _has_cb_clause_deleted {false};
    std::function<void(Mallob::Clause&)> _cb_clause_deleted;

public:
    struct Setup {
        int numLiterals = 1000;
        int maxClauseLength = 20;
        int maxLbdPartitionedSize = 2;
        bool useChecksums = false;
        bool slotsForSumOfLengthAndLbd = false;
    };

    PriorityClauseBuffer(Setup setup) :
        _total_literal_limit(setup.numLiterals),
        _max_lbd_partitioned_size(setup.maxLbdPartitionedSize),
        _max_clause_length(setup.maxClauseLength),
        _slots_for_sum_of_length_and_lbd(setup.slotsForSumOfLengthAndLbd),
        _use_checksum(setup.useChecksums),
        _bucket_iterator(setup.slotsForSumOfLengthAndLbd ? 
            BucketLabel::MINIMIZE_SUM_OF_SIZE_AND_LBD : BucketLabel::MINIMIZE_SIZE, 
            setup.maxLbdPartitionedSize),
        _hist_deleted_in_slots(setup.maxClauseLength) {

        // Choose max. sum such that the largest legal clauses will be admitted iff they have LBD 2. 
        int maxSumOfLengthAndLbd = setup.maxClauseLength+2;

        // Iterate over all possible clause length - LBD combinations
        for (int clauseLength = 1; clauseLength <= setup.maxClauseLength; clauseLength++) {
            for (int lbd = std::min(clauseLength, 2); lbd <= clauseLength; lbd++) {

                std::pair<int, int> lengthLbdPair(clauseLength, lbd);
                std::pair<int, int> representantKey;
                ClauseSlotMode opMode;

                // Decide what kind of slot we need for this combination
                int sumOfLengthAndLbd = clauseLength+lbd;
                if (setup.slotsForSumOfLengthAndLbd && sumOfLengthAndLbd >= 6 
                        && sumOfLengthAndLbd <= maxSumOfLengthAndLbd) {
                    // Shared slot for all clauses of this length+lbd sum
                    opMode = ClauseSlotMode::SAME_SUM_OF_SIZE_AND_LBD;
                    representantKey = std::pair<int, int>(sumOfLengthAndLbd-2, 2);
                } else if (clauseLength > setup.maxLbdPartitionedSize) {
                    // Slot for all clauses of this length
                    opMode = ClauseSlotMode::SAME_SIZE;
                    representantKey = std::pair<int, int>(clauseLength, clauseLength);
                } else {
                    // Exclusive slot for this length-lbd combination
                    opMode = ClauseSlotMode::SAME_SIZE_AND_LBD;
                    representantKey = lengthLbdPair;
                }

                if (!_size_lbd_to_slot_idx_mode.count(representantKey)) {
                    // Create new slot
                    int slotIdx = _slots.size();
                    _slots.emplace_back(new ClauseSlot(_free_budget, slotIdx, clauseLength, opMode == SAME_SIZE_AND_LBD ? lbd : 0));
                    if (_slots.size() > 1) _slots.back()->setLeftNeighbor(_slots[_slots.size()-2].get());
                    _size_lbd_to_slot_idx_mode[representantKey] = std::pair<int, ClauseSlotMode>(slotIdx, opMode);
                }

                // May be a no-op
                auto val = _size_lbd_to_slot_idx_mode[representantKey];
                _size_lbd_to_slot_idx_mode[lengthLbdPair] = val;
                //LOG(V2_INFO, "SLOT (%i,%i) ~> %i\n", lengthLbdPair.first, lengthLbdPair.second, _size_lbd_to_slot_idx_mode[representantKey].first);
            }
        }

        // Store initial literal budget
        _free_budget.store(_total_literal_limit, std::memory_order_relaxed);
    }
    ~PriorityClauseBuffer() {}

    bool addClause(const Clause& c) {
        auto [slotIdx, mode] = getSlotIdxAndMode(c.size, c.lbd);
        return _slots[slotIdx]->insert(c, _slots.back().get());
    }

    bool addClause(int* cBegin, int cSize, int cLbd) {
        return addClause(Mallob::Clause(cBegin, cSize, cLbd));
    }

    template <typename T>
    void addClauses(BufferReader& inputBuffer) {
        for (auto& slot : _slots) slot->insert(inputBuffer, _slots.back().get());
    }
    
    enum ExportMode {UNITS, ANY};
    std::vector<int> exportBuffer(int sizeLimit, int& numExportedClauses, 
            ExportMode mode = ANY, bool sortClauses = true, 
            std::function<void(int*)> clauseDataConverter = [](int*){}) {
        
        BufferBuilder builder(sizeLimit, _max_clause_length, _slots_for_sum_of_length_and_lbd);
        if (mode == ANY) {
            for (auto& slot : _slots) slot->flushAndShrink(builder, clauseDataConverter);
        }
        if (mode == UNITS) {
            auto [unitSlotIdx, _] = getSlotIdxAndMode(1, 1);
            _slots[unitSlotIdx]->flushAndShrink(builder, clauseDataConverter);
        }
        numExportedClauses = builder.getNumAddedClauses();
        return builder.extractBuffer();
    }

    BufferReader getBufferReader(int* begin, size_t size, bool useChecksums = false) {
        return BufferReader(begin, size, _max_clause_length, _slots_for_sum_of_length_and_lbd, useChecksums);
    }

    BufferMerger getBufferMerger(int sizeLimit) {
        return BufferMerger(sizeLimit, _max_clause_length, _slots_for_sum_of_length_and_lbd, _use_checksum);
    }

    BufferBuilder getBufferBuilder(std::vector<int>* out) {
        return BufferBuilder(-1, _max_clause_length, _slots_for_sum_of_length_and_lbd, out);
    }

    int getCurrentlyUsedLiterals() const {
        return _total_literal_limit - _free_budget.load(std::memory_order_relaxed);
    }
    int getNumLiterals(int clauseLength, int lbd) {
        auto [slotIdx, mode] = getSlotIdxAndMode(clauseLength, lbd);
        return _slots[slotIdx]->getNbStoredLiterals();
    }
    int getTotalLiteralBudget() const {
        return _total_literal_limit;
    }

    ClauseHistogram& getDeletedClausesHistogram() {
        return _hist_deleted_in_slots;
    }

    bool checkTotalLiterals() {
        // TODO
        return true;
    }

    void setClauseDeletionCallback(std::function<void(Mallob::Clause&)> cb) {
        _has_cb_clause_deleted = true;
        _cb_clause_deleted = cb;
        for (auto& slot : _slots) slot->setDiscardedClausesNotification(_cb_clause_deleted, _hist_deleted_in_slots);
    }

    void clearClauseDeletionCallback() {
        _has_cb_clause_deleted = false;
    }

private:
    std::pair<int, ClauseSlotMode> getSlotIdxAndMode(int clauseSize, int lbd) {
        assert(lbd >= 1);
        assert(clauseSize == 1 || lbd >= 2 || log_return_false("(%i,%i) invalid length-clause combination!\n", clauseSize, lbd));
        assert(lbd <= clauseSize);
        auto pair = std::pair<int, int>(clauseSize, lbd);
        return _size_lbd_to_slot_idx_mode.at(pair);
    }
};
