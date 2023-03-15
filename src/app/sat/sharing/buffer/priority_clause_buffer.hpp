
#pragma once

#include <atomic>
#include <list>
#include <limits>
#include <memory>
#include <numeric>


#include "../../data/produced_clause.hpp"
#include "app/sat/data/clause_histogram.hpp"
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
    const int UNIT_SLOT_MAX_BUDGET {std::numeric_limits<int>::max() - 1024};
    std::atomic_int _unit_slot_budget {UNIT_SLOT_MAX_BUDGET};

    std::atomic_int _max_admissible_slot_idx;

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

    unsigned long _op_count {1};
    unsigned long _cached_op_count {0};
    int _cached_pop_slot {0};

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
                    _slots.emplace_back(new ClauseSlot(clauseLength == 1 ? _unit_slot_budget : _free_budget, 
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
    ~PriorityClauseBuffer() {}

    bool addClause(const Clause& c) {
        ++_op_count;
        auto [slotIdx, mode] = getSlotIdxAndMode(c.size, c.lbd);
        int maxSlotIdx = _max_admissible_slot_idx.load(std::memory_order_relaxed);
        if (slotIdx > maxSlotIdx) return false;
        return _slots[slotIdx]->insert(c, _slots[maxSlotIdx].get());
    }

    bool addClause(int* cBegin, int cSize, int cLbd) {
        return addClause(Mallob::Clause(cBegin, cSize, cLbd));
    }

    void addClauses(BufferReader& inputBuffer, ClauseHistogram* hist) {
        ++_op_count;
        auto& clause = inputBuffer.getNextIncomingClause(); // prepare first clause in reader
        int maxSlotIdx = _max_admissible_slot_idx.load(std::memory_order_relaxed);
        for (int slotIdx = 0; slotIdx < _slots.size(); slotIdx++) {
            auto& slot = _slots[slotIdx];
            if (!slot->fitsThisSlot(clause)) continue;
            int nbInsertedCls = slotIdx > maxSlotIdx ? 0 : slot->insert(inputBuffer, _slots[maxSlotIdx].get());
            //LOG(V2_INFO, "DBG -- (%i,?) : %i clauses inserted\n", slot->getClauseLength(), nbInsertedCls);
            if (hist) hist->increase(slot->getClauseLength(), nbInsertedCls);
        }
    }
    
    enum ExportMode {UNITS, NONUNITS, ANY};
    bool popClauseWeak(ExportMode mode, Mallob::Clause& clause) {
        int slotIdx = mode == NONUNITS ? 1 : 0;
        if (_op_count == _cached_op_count) {
            // No changes since last operation! Can use cached slot index.
            slotIdx = std::max(slotIdx, _cached_pop_slot);
        }
        ++_op_count;
        for (; slotIdx < _slots.size(); slotIdx++) {
            if (mode == UNITS && slotIdx > 0) return false;
            if (_slots[slotIdx]->popWeak(clause)) {
                // Remember this slot for the next pop operation
                _cached_op_count = _op_count;
                _cached_pop_slot = slotIdx;
                return true;
            }
        }
        return false;
    }

    std::vector<int> exportBuffer(int sizeLimit, int& numExportedClauses, 
            ExportMode mode = ANY, bool sortClauses = true, 
            std::function<void(int*)> clauseDataConverter = [](int*){}) {
        
        ++_op_count;
        BufferBuilder builder(sizeLimit, _max_clause_length, _slots_for_sum_of_length_and_lbd);

        if (mode != NONUNITS) {
            _slots[0]->flushAndShrink(builder, clauseDataConverter);
        }
        if (mode != UNITS) {
            _max_admissible_slot_idx.store(_slots.size()-1, std::memory_order_relaxed);
            bool updateMaxAdmissibleIndex = true;
            int nbLitsEncountered = 0;
            for (int i = 1; i < _slots.size(); i++) {
                if (updateMaxAdmissibleIndex) {
                    nbLitsEncountered += _slots[i]->getNbStoredLiterals();
                    if (nbLitsEncountered >= 0.95 * _total_literal_limit) {
                        _max_admissible_slot_idx.store(i, std::memory_order_relaxed);
                        //LOG(V2_INFO, "LIMIT pcb adm. slot to %i\n", i);
                        updateMaxAdmissibleIndex = false;
                    }
                }
                _slots[i]->flushAndShrink(builder, clauseDataConverter);
            }
        }
        numExportedClauses = builder.getNumAddedClauses();
        return builder.extractBuffer();
    }

    BufferReader getBufferReader(int* begin, size_t size, bool useChecksums = false) const {
        return BufferReader(begin, size, _max_clause_length, _slots_for_sum_of_length_and_lbd, useChecksums);
    }

    BufferMerger getBufferMerger(int sizeLimit) const {
        return BufferMerger(sizeLimit, _max_clause_length, _slots_for_sum_of_length_and_lbd, _use_checksum);
    }

    BufferBuilder getBufferBuilder(std::vector<int>* out) const {
        return BufferBuilder(-1, _max_clause_length, _slots_for_sum_of_length_and_lbd, out);
    }

    int getCurrentlyUsedLiterals() const {
        return (_total_literal_limit - _free_budget.load(std::memory_order_relaxed))
            + (UNIT_SLOT_MAX_BUDGET - _unit_slot_budget.load(std::memory_order_relaxed));
    }
    std::string getCurrentlyUsedLiteralsReport() const {
        std::string out;
        for (auto& slot : _slots) out += std::to_string(slot->getNbStoredLiterals()) + " ";
        return out;
    }
    int getNumLiterals(int clauseLength, int lbd) const {
        auto [slotIdx, mode] = getSlotIdxAndMode(clauseLength, lbd);
        return _slots[slotIdx]->getNbStoredLiterals();
    }

    ClauseHistogram& getDeletedClausesHistogram() {
        return _hist_deleted_in_slots;
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

    void setClauseDeletionCallback(std::function<void(Mallob::Clause&)> cb) {
        _has_cb_clause_deleted = true;
        _cb_clause_deleted = cb;
        for (auto& slot : _slots) slot->setDiscardedClausesNotification(_cb_clause_deleted, _hist_deleted_in_slots);
    }

    void clearClauseDeletionCallback() {
        _has_cb_clause_deleted = false;
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
