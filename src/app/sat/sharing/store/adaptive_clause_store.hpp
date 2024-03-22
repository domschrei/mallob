
#pragma once

#include <atomic>
#include <list>
#include <limits>
#include <memory>
#include <numeric>

#include "../../data/produced_clause.hpp"
#include "app/sat/data/clause_histogram.hpp"
#include "app/sat/sharing/buffer/buffer_merger.hpp"
#include "app/sat/sharing/store/generic_clause_store.hpp"
#include "bucket_label.hpp"
#include "util/logger.hpp"
#include "util/periodic_event.hpp"
#include "../../data/solver_statistics.hpp"
#include "clause_slot.hpp"

class AdaptiveClauseStore : public GenericClauseStore {

private:
    int _total_literal_limit;
    std::vector<std::unique_ptr<ClauseSlot>> _slots;
    std::atomic_int _free_budget {0};
    const int UNIT_SLOT_MAX_BUDGET {std::numeric_limits<int>::max() - 1024};
    std::atomic_int _infinite_budget {UNIT_SLOT_MAX_BUDGET};

    std::atomic_int _max_admissible_slot_idx;

    enum ClauseSlotMode {SAME_SUM_OF_SIZE_AND_LBD, SAME_SIZE, SAME_SIZE_AND_LBD};
    robin_hood::unordered_flat_map<std::pair<int, int>, std::pair<int, ClauseSlotMode>, IntPairHasher> _size_lbd_to_slot_idx_mode;

    int _max_lbd_partitioned_size;
    int _max_eff_clause_length;
    int _max_free_eff_clause_length;
    bool _slots_for_sum_of_length_and_lbd;

    bool _use_checksum;
    BucketLabel _bucket_iterator;

    int _next_pop_idx {0};
    bool _reset_pop_idx {false};
    unsigned long _pop_op_count {1};

public:
    struct Setup {
        int numLiterals = 1000;
        int maxEffectiveClauseLength = 20;
        int maxFreeEffectiveClauseLength = 1;
        int maxLbdPartitionedSize = 2;
        bool useChecksums = false;
        bool slotsForSumOfLengthAndLbd = false;
        bool resetLbdAtExport = false;
    };

    AdaptiveClauseStore(Setup setup) :
        GenericClauseStore(setup.maxEffectiveClauseLength, setup.resetLbdAtExport),
        _total_literal_limit(setup.numLiterals),
        _max_lbd_partitioned_size(setup.maxLbdPartitionedSize),
        _max_eff_clause_length(setup.maxEffectiveClauseLength),
        _max_free_eff_clause_length(setup.maxFreeEffectiveClauseLength),
        _slots_for_sum_of_length_and_lbd(setup.slotsForSumOfLengthAndLbd),
        _use_checksum(setup.useChecksums),
        _bucket_iterator(setup.slotsForSumOfLengthAndLbd ? 
            BucketLabel::MINIMIZE_SUM_OF_SIZE_AND_LBD : BucketLabel::MINIMIZE_SIZE, 
            setup.maxLbdPartitionedSize) {

        // Choose max. sum such that the largest legal clauses will be admitted iff they have LBD 2. 
        int maxSumOfLengthAndLbd = setup.maxEffectiveClauseLength+2;

        // Iterate over all possible clause length - LBD combinations
        for (int clauseLength = 1; clauseLength <= setup.maxEffectiveClauseLength; clauseLength++) {
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
                    _slots.emplace_back(new ClauseSlot(clauseLength <= _max_free_eff_clause_length ? _infinite_budget : _free_budget, 
                        slotIdx, clauseLength, opMode == SAME_SIZE_AND_LBD ? lbd : 0));
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
        _max_admissible_slot_idx.store(_slots.size()-1, std::memory_order_relaxed);
    }
    ~AdaptiveClauseStore() {}

    bool addClause(const Mallob::Clause& c) override {
        auto [slotIdx, mode] = getSlotIdxAndMode(c.size, c.lbd);
        int maxSlotIdx = _max_admissible_slot_idx.load(std::memory_order_relaxed);
        if (slotIdx > maxSlotIdx) return false;
        auto result = _slots[slotIdx]->insert(c, _slots[maxSlotIdx].get());
        _reset_pop_idx = true;
        return result;
    }

    bool addClause(int* cBegin, int cSize, int cLbd) {
        return addClause(Mallob::Clause(cBegin, cSize, cLbd));
    }

    void addClauses(BufferReader& inputBuffer, ClauseHistogram* hist) override {
        auto& clause = inputBuffer.getNextIncomingClause(); // prepare first clause in reader
        int maxSlotIdx = _max_admissible_slot_idx.load(std::memory_order_relaxed);
        for (int slotIdx = 0; slotIdx < _slots.size(); slotIdx++) {
            auto& slot = _slots[slotIdx];
            if (!slot->fitsThisSlot(clause)) continue;
            int nbInsertedCls = slotIdx > maxSlotIdx ? 0 : slot->insert(inputBuffer, _slots[maxSlotIdx].get());
            //LOG(V2_INFO, "DBG -- (%i,?) : %i clauses inserted\n", slot->getClauseLength(), nbInsertedCls);
            if (hist) hist->increase(slot->getClauseLength(), nbInsertedCls);
            if (nbInsertedCls > 0) _reset_pop_idx = true;
        }
    }
    
    bool popClauseWeak(ExportMode mode, Mallob::Clause& clause) {

        // Check if any clauses for the desired export mode are available
        auto sizeNonunit = getCurrentlyUsedNonunitLiterals();
        auto sizeUnit = getCurrentlyUsedUnitLiterals();
        if (mode == NONUNITS && sizeNonunit == 0) return false;
        if (mode == UNITS && sizeUnit == 0) return false;
        if (mode == ANY && sizeUnit+sizeNonunit == 0) return false;

        // Find slot index to begin at
        //LOG(V4_VVER, "POP NONEMPTY\n");
        int slotIdx = mode == NONUNITS ? 1 : 0;
        if (mode != UNITS) {
            if (_reset_pop_idx) {
                //LOG(V4_VVER, "RESET POP IDX %i\n", _next_pop_idx);
                _next_pop_idx = 0;
                _reset_pop_idx = false;
            }
            slotIdx = std::max(slotIdx, _next_pop_idx);
        }
        if (_pop_op_count % 131072 == 0) {
            // Perform a flush over all slots to shrink slots and discard old clauses
            BufferBuilder dummyBuilder = getBufferBuilder(nullptr, 0);
            for (auto& slot : _slots) slot->flushAndShrink(dummyBuilder);
            assert(dummyBuilder.getNumAddedClauses() == 0);
        }
        // Cycle once over all slots, beginning with the (cached) slot index,
        // until success. Remember if a spurious fail occurred somewhere.
        bool spuriousFails = false;
        for (; slotIdx < _slots.size(); slotIdx++) {
            if (mode == UNITS && slotIdx != 0) break;
            if (mode == NONUNITS && slotIdx == 0) continue;
            auto result = _slots[slotIdx]->popWeak(clause);
            if (result == ClauseSlot::SUCCESS) {
                // Remember this slot for the next pop operation.
                // There must not have been any spurious fails 
                // (otherwise nonempty slots might have been skipped).
                if (!spuriousFails) {
                    //if (_next_pop_idx != slotIdx) LOG(V4_VVER, "POP SLOT IDX %i ~> %i\n", _next_pop_idx, slotIdx);
                    _next_pop_idx = slotIdx;
                }
                _pop_op_count++;
                return true;
            } else if (result == ClauseSlot::SPURIOUS_FAIL) {
                //if (_next_pop_idx != slotIdx) LOG(V4_VVER, "POP SLOT IDX %i ~> %i\n", _next_pop_idx, slotIdx);
                _next_pop_idx = slotIdx;
                spuriousFails = true;
            }
        }
        return false;
    }

    std::vector<int> exportBuffer(int sizeLimit, int& numExportedClauses, int& numExportedLits,
            ExportMode mode = ANY, bool sortClauses = true, 
            std::function<void(int*)> clauseDataConverter = [](int*){}) override {

        BufferBuilder builder(sizeLimit, _max_eff_clause_length, _slots_for_sum_of_length_and_lbd);
        builder.setFreeClauseLengthLimit(_max_free_eff_clause_length);

        if (mode != NONUNITS) {
            _slots[0]->flushAndShrink(builder, clauseDataConverter);
        }
        if (mode != UNITS) {
            _max_admissible_slot_idx.store(_slots.size()-1, std::memory_order_relaxed);
            int nbLitsContained = getCurrentlyUsedNonunitLiterals();
            bool updateMaxAdmissibleIndex = nbLitsContained >= 0.9*_total_literal_limit;
            int nbLitsEncountered = 0;
            ClauseSlot::FlushMode flushMode {ClauseSlot::FLUSH_FITTING};
            for (int i = 1; i < _slots.size(); i++) {
                // Get number of a priori stored literals, flush slot
                nbLitsEncountered += _slots[i]->getNbStoredLiterals();
                _slots[i]->flushAndShrink(builder, clauseDataConverter, flushMode, _reset_lbd_at_export);
                // Enough literals encountered to make the cut for updating max. admissible slot?
                if (updateMaxAdmissibleIndex && nbLitsEncountered >= 0.95 * nbLitsContained) {
                    //LOG(V2_INFO, "LIMIT pcb adm. slot to %i\n", i);
                    // Update maximum admissible slot index
                    _max_admissible_slot_idx.store(i, std::memory_order_relaxed);
                    updateMaxAdmissibleIndex = false;
                    // Clear all clauses from all subsequent slots
                    flushMode = ClauseSlot::FLUSH_OR_DISCARD_ALL;
                }
            }
        }
        numExportedClauses = builder.getNumAddedClauses();
        numExportedLits = builder.getNumAddedLits();
        return builder.extractBuffer();
    }

    BufferReader getBufferReader(int* begin, size_t size, bool useChecksums = false) const override {
        return BufferReader(begin, size, _max_eff_clause_length, _slots_for_sum_of_length_and_lbd, useChecksums);
    }

    BufferMerger getBufferMerger(int sizeLimit) const {
        return BufferMerger(sizeLimit, _max_eff_clause_length, 0, _slots_for_sum_of_length_and_lbd, _use_checksum);
    }

    BufferBuilder getBufferBuilder(std::vector<int>* out, int totalLiteralLimit = -1) const {
        return BufferBuilder(totalLiteralLimit, _max_eff_clause_length, _slots_for_sum_of_length_and_lbd, out);
    }

    int getCurrentlyUsedLiterals() const override {
        return getCurrentlyUsedNonunitLiterals() + getCurrentlyUsedUnitLiterals();
    }
    int getCurrentlyUsedNonunitLiterals() const {
        return (_total_literal_limit - _free_budget.load(std::memory_order_relaxed));
    }
    int getCurrentlyUsedUnitLiterals() const {
        return (UNIT_SLOT_MAX_BUDGET - _infinite_budget.load(std::memory_order_relaxed));
    }
    std::string getCurrentlyUsedLiteralsReport() const override {
        std::string out;
        for (auto& slot : _slots) out += std::to_string(slot->getNbStoredLiterals()) + " ";
        return out;
    }
    int getNumLiterals(int clauseLength, int lbd) const {
        auto [slotIdx, mode] = getSlotIdxAndMode(clauseLength, lbd);
        return _slots[slotIdx]->getNbStoredLiterals();
    }

    bool checkTotalLiterals() {
        int nbLiterals = 0;
        for (auto& slot : _slots) nbLiterals += slot->getNbStoredLiterals();
        int nbAdvertisedLiterals = getCurrentlyUsedLiterals();
        if (nbLiterals != nbAdvertisedLiterals) {
            LOG(V0_CRIT, "[ERROR] %i literals advertised, %i present\n", nbAdvertisedLiterals, nbLiterals);
            return false;
        }
        return true;
    }

    void setClauseDeletionCallback(int clauseLength, std::function<void(Mallob::Clause&)> cb) override {
        // Iterate over all distinct slots with the specified clause length
        int lastSlotIdx = -1;
        for (int lbd = std::min(clauseLength, 2); lbd <= clauseLength; lbd++) {
            int slotIdx = getSlotIdxAndMode(clauseLength, lbd).first;
            if (slotIdx != lastSlotIdx) {
                lastSlotIdx = slotIdx;
                _slots[slotIdx]->setDiscardedClausesNotification(cb, _hist_deleted_in_slots);
            }
        }
    }

    int getMaxAdmissibleEffectiveClauseLength() const override {
        int slotIdx = _max_admissible_slot_idx.load(std::memory_order_relaxed);
        assert(slotIdx >= 0 && slotIdx < _slots.size());
        return _slots[slotIdx]->getClauseLength();
    }

private:
    std::pair<int, ClauseSlotMode> getSlotIdxAndMode(int clauseSize, int lbd) const {
        assert(lbd >= 1);
        assert(clauseSize == 1 || lbd >= 2 || log_return_false("(%i,%i) invalid length-clause combination!\n", clauseSize, lbd));
        assert(lbd <= clauseSize);
        auto pair = std::pair<int, int>(clauseSize, lbd);
        return _size_lbd_to_slot_idx_mode.at(pair);
    }
};
