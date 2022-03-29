
#include "adaptive_clause_database.hpp"

AdaptiveClauseDatabase::AdaptiveClauseDatabase(Setup setup):
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
                int slotIdx;
                if (clauseLength == 1) {
                    slotIdx = -2;
                    _unit_slot.implicitLbdOrZero = 1;
                    _unit_slot.mtx.reset(new Mutex());
                } else if (clauseLength == 2) {
                    slotIdx = -1;
                    _binary_slot.implicitLbdOrZero = 2;
                    _binary_slot.mtx.reset(new Mutex());
                } else {
                    slotIdx = _large_slots.size();
                    _large_slots.emplace_back();
                    _large_slots.back().implicitLbdOrZero = (opMode == SAME_SIZE_AND_LBD ? lbd : 0);
                    _large_slots.back().mtx.reset(new Mutex());
                }
                _size_lbd_to_slot_idx_mode[representantKey] = std::pair<int, ClauseSlotMode>(slotIdx, opMode);
            }

            // May be a no-op
            auto val = _size_lbd_to_slot_idx_mode[representantKey];
            _size_lbd_to_slot_idx_mode[lengthLbdPair] = val;
            //LOG(V2_INFO, "SLOT (%i,%i) ~> %i\n", lengthLbdPair.first, lengthLbdPair.second, _size_lbd_to_slot_idx[representantKey]);
        }
    }

    // Indicate initial literal budget
    _num_free_literals.store(setup.numLiterals, std::memory_order_relaxed);
}

bool AdaptiveClauseDatabase::addClause(const Clause& c) {
    return addClause(c.begin, c.size, c.lbd);
}

bool AdaptiveClauseDatabase::addClause(int* cBegin, int cSize, int cLbd, bool sortLargeClause) {

    int len = cSize;
    auto [slotIdx, mode] = getSlotIdxAndMode(len, cLbd);
    bool acquired = tryAcquireBudget(len);

    if (!acquired) {
        // Try acquire budget through stealing from a less important slot
        int freed = freeLowPriorityLiterals(slotIdx);
        // Freed enough memory?
        if (freed >= len) {
            // Steal successful: use len freed lits for this clause implicitly
            acquired = true;
            // Release the freed budget which you DO NOT need for this clause insertion
            if (freed != len)
                _num_free_literals.fetch_add(freed-len, std::memory_order_relaxed);
            
        } else if (freed > 0) {
            // Return insufficient freed budget to global storage
            _num_free_literals.fetch_add(freed, std::memory_order_relaxed);
        }
        
        // Retry acquisition explicitly, if necessary
        acquired = acquired || tryAcquireBudget(len);
    }
    if (!acquired) return false;

    // Budget has been acquired successfully    
    
    if (cSize == 1) {
        auto lock = _unit_slot.mtx->getLock();
        _unit_slot.list.push_front(*cBegin);
        _unit_slot.empty = false;
    } else if (cSize == 2) {
        auto lock = _binary_slot.mtx->getLock();
        _binary_slot.list.emplace_front(cBegin[0], cBegin[1]);
        _binary_slot.empty = false;
    } else {
        // Sort clause if necessary
        if (sortLargeClause) std::sort(cBegin, cBegin+cSize);
        // Insert clause
        auto& slot = _large_slots.at(slotIdx);
        bool explicitLbd = slot.implicitLbdOrZero == 0;
        std::vector<int> vec(len + (explicitLbd ? 1 : 0));
        size_t i = 0;
        if (explicitLbd) vec[i++] = cLbd;
        for (size_t j = 0; j < len; j++) vec[i++] = cBegin[j];
        auto lock = slot.mtx->getLock();
        slot.list.push_front(std::move(vec));
        slot.empty = false;
    }
    return true;
}

Mallob::Clause AdaptiveClauseDatabase::popFront(ExportMode mode) {

    if (mode != ExportMode::NONUNITS) {
        auto c = popMallobClause(_unit_slot);
        if (c.begin != nullptr) return c;
    }
    if (mode == ExportMode::UNITS) return Mallob::Clause();

    {
        auto c = popMallobClause(_binary_slot);
        if (c.begin != nullptr) return c;
    }

    for (int slotIdx = 0; slotIdx < _large_slots.size(); slotIdx++) {
        auto& slot = _large_slots[slotIdx];
        auto c = popMallobClause(slot);
        if (c.begin != nullptr) return c;
    }

    return Mallob::Clause();
}

template <typename T>
Mallob::Clause AdaptiveClauseDatabase::popMallobClause(Slot<T>& slot) {
    if (slot.empty) return Mallob::Clause();
    auto lock = slot.mtx->getLock();
    if (slot.empty) return Mallob::Clause();
    T packed = std::move(slot.list.front());
    slot.list.pop_front();
    if (slot.list.empty()) slot.empty = true;
    auto mc = getMallobClause(packed, slot.implicitLbdOrZero);
    _num_free_literals.fetch_add(mc.size, std::memory_order_relaxed);
    return mc.copy();
}

template <typename T>
Mallob::Clause AdaptiveClauseDatabase::getMallobClause(T& elem, int implicitLbdOrZero) {
    if constexpr (std::is_same<int, T>::value) {
        return Mallob::Clause(&elem, 1, 1);
    }
    if constexpr (std::is_same<std::pair<int, int>, T>::value) {
        return Mallob::Clause(&elem.first, 2, 2);
    }
    if constexpr (std::is_same<std::vector<int>, T>::value) {
        bool lbdInVector = implicitLbdOrZero == 0;
        int len = elem.size() - (lbdInVector ? 1 : 0);
        assert(len <= _max_clause_length);
        return Mallob::Clause(elem.data() + (lbdInVector ? 1 : 0), len, 
            lbdInVector ? elem[0] : implicitLbdOrZero);
    }
    abort();
}

template <typename T> 
void AdaptiveClauseDatabase::flushClauses(Slot<T>& slot, bool sortClauses, BufferBuilder& builder) {
    
    if (slot.empty) return;
    
    // Swap current clause information in the slot to another list
    std::forward_list<T> swappedSlot;
    {
        auto lock = slot.mtx->getLock();
        if (slot.empty) return;
        swappedSlot.swap(slot.list);
        slot.empty = true;
    }

    // Create clauses one by one
    std::vector<Mallob::Clause> flushedClauses;
    int remainingLits = builder.getMaxRemainingLits();
    int collectedLits = 0;
    auto it = swappedSlot.begin();
    for (; it != swappedSlot.end(); ++it) {
        T& elem = *it;
        Mallob::Clause clause = getMallobClause(elem, slot.implicitLbdOrZero);
        if (clause.size > remainingLits) break;

        // insert clause to export buffer
        remainingLits -= clause.size;
        collectedLits += clause.size;
        flushedClauses.push_back(std::move(clause));
    }

    // Return budget of extracted literals
    _num_free_literals.fetch_add(collectedLits, std::memory_order_relaxed); 

    if (it != swappedSlot.end()) {
        // Re-insert swapped clauses which remained unused
        auto lock = slot.mtx->getLock();
        slot.list.splice_after(slot.list.before_begin(), swappedSlot, it, swappedSlot.end());
        slot.empty = slot.list.empty();
    }

    bool differentLbdValues = slot.implicitLbdOrZero == 0;
    if (differentLbdValues || sortClauses) {
        // Sort
        std::sort(flushedClauses.begin(), flushedClauses.end());
    }

    // Append clause to buffer builder
    for (auto& c : flushedClauses) {
        bool success = builder.append(c);
        assert(success);
        //log(V2_INFO, "%i : EXPORTED %s\n", producedClause.producers, c.toStr().c_str());
    }
}

std::vector<int> AdaptiveClauseDatabase::exportBuffer(int totalLiteralLimit, int& numExportedClauses, 
        ExportMode mode, bool sortClauses) {

    BufferBuilder builder(totalLiteralLimit, _max_clause_length, _slots_for_sum_of_length_and_lbd);

    if (mode != ExportMode::NONUNITS) {
        // Export unit clauses.
        flushClauses(_unit_slot, sortClauses, builder);
    }
    if (mode != ExportMode::UNITS) {
        // Export all other clauses.

        // Binary clauses first.
        flushClauses(_binary_slot, sortClauses, builder);

        // All other clauses.
        for (int slotIdx = 0; slotIdx < _large_slots.size(); slotIdx++) {
            auto& slot = _large_slots[slotIdx];
            flushClauses(slot, sortClauses, builder);
        }
    }

    numExportedClauses = builder.getNumAddedClauses();
    return builder.extractBuffer();
}

int AdaptiveClauseDatabase::freeLowPriorityLiterals(int callingSlot) {
    int numFreed = 0;
    int numDesired = 5*_max_clause_length;
    int lb = std::max(-1, callingSlot);
    int ub = _large_slots.size()-1;

    for (int slotIdx = ub; numFreed < numDesired && slotIdx > lb; slotIdx--) {
        auto& slot = _large_slots[slotIdx];
        numFreed += dropClauses(slot, numDesired-numFreed);
        //if (numLiterals > 0) LOG(V2_INFO, "%i -> %i : Freed %i lits\n", callingSlot, slotIdx, numLiterals);
    }

    if (numFreed < numDesired && callingSlot == -2) {
        numFreed += dropClauses(_binary_slot, numDesired-numFreed);
        //if (numLiterals > 0) LOG(V2_INFO, "%i -> %i : Freed %i lits\n", callingSlot, -2, numLiterals);
    }

    return numFreed;
}

template <typename T>
int AdaptiveClauseDatabase::dropClauses(Slot<T>& slot, int desiredLiterals) {
    
    if (slot.empty) return 0;

    std::forward_list<T> swappedList;
    int numCollectedLits = 0;

    auto lock = slot.mtx->getLock();
    auto it = slot.list.begin();
    while (numCollectedLits < desiredLiterals && it != slot.list.end()) {
        if constexpr (std::is_same<int, T>::value) {
            ++numCollectedLits;
            _hist_deleted_in_slots.increment(1);
        }
        if constexpr (std::is_same<std::pair<int, int>, T>::value) {
            numCollectedLits += 2;
            _hist_deleted_in_slots.increment(2);
        }
        if constexpr (std::is_same<std::vector<int>, T>::value) {
            int clslen = it->size() - (slot.implicitLbdOrZero==0 ? 1 : 0);
            numCollectedLits += clslen;
            _hist_deleted_in_slots.increment(clslen);
        }
        ++it;
    }

    if (numCollectedLits == 0) return 0;
    if (it == slot.list.end()) {
        // Swap the entire list
        swappedList.swap(slot.list);
        slot.empty = true;
    } else {
        // Extract part of the list
        swappedList.splice_after(swappedList.before_begin(), slot.list, slot.list.begin(), it);
    }

    // Elements are cleaned up automatically
    return numCollectedLits;
}

BufferReader AdaptiveClauseDatabase::getBufferReader(int* begin, size_t size, bool useChecksums) {
    return BufferReader(begin, size, _max_clause_length, _slots_for_sum_of_length_and_lbd, useChecksums);
}

BufferMerger AdaptiveClauseDatabase::getBufferMerger(int sizeLimit) {
    return BufferMerger(sizeLimit, _max_clause_length, _slots_for_sum_of_length_and_lbd, _use_checksum);
}

BufferBuilder AdaptiveClauseDatabase::getBufferBuilder(std::vector<int>* out) {
    return BufferBuilder(-1, _max_clause_length, _slots_for_sum_of_length_and_lbd, out);
}

ClauseHistogram& AdaptiveClauseDatabase::getDeletedClausesHistogram() {
    return _hist_deleted_in_slots;
}

std::pair<int, AdaptiveClauseDatabase::ClauseSlotMode> AdaptiveClauseDatabase::getSlotIdxAndMode(int clauseSize, int lbd) {
    assert(lbd >= 1);
    assert(clauseSize == 1 || lbd >= 2 || log_return_false("(%i,%i) invalid length-clause combination!\n", clauseSize, lbd));
    assert(lbd <= clauseSize);
    auto pair = std::pair<int, int>(clauseSize, lbd);
    return _size_lbd_to_slot_idx_mode.at(pair);
}

BucketLabel AdaptiveClauseDatabase::getBucketIterator() {
    return _bucket_iterator;
}

bool AdaptiveClauseDatabase::tryAcquireBudget(int len) {
    int budget = _num_free_literals.load(std::memory_order_relaxed);
    while (budget >= len && !_num_free_literals.compare_exchange_strong(budget, budget-len, std::memory_order_relaxed)) {}
    return budget >= len;
}

/*
void AdaptiveClauseDatabase::printChunks(int nextExportSize) {
    size_t i = 0;
    BucketLabel l;
    int virtuallyExported = 0;
    for (auto& slot : _slots) {
        int effectiveClauseSize = l.size + (l.size > _max_lbd_partitioned_size ? 1 : 0);
        int capacityPerChunk = _chunk_size / effectiveClauseSize;
        auto fillStates = slot.getChunkFillStates();
        std::string out = "";
        for (size_t i = 0; i < fillStates.size(); i++) {
            
            int numElems = fillStates[i];
            if (numElems == -1) {
                out += "  ---   ";
                continue;
            }

            out += "[";
            
            float fillRatio = ((float)numElems) / capacityPerChunk;
            bool readFrom = (nextExportSize >= 0 && virtuallyExported < nextExportSize);
            virtuallyExported += numElems * effectiveClauseSize;
            bool readAll = (nextExportSize >= 0 && virtuallyExported <= nextExportSize);
            if (readAll) out += "\033[34m";
            else if (readFrom) out += "\033[36m";

            if (fillRatio <= 0)   out += ".....";
            else if (fillRatio <= 0.2) out += "|....";
            else if (fillRatio <= 0.4) out += "||...";
            else if (fillRatio <= 0.6) out += "|||..";
            else if (fillRatio <= 0.8) out += "||||.";
            else                       out += "|||||";
            if (nextExportSize >= 0) out += "\033[0m";
            
            out += "] ";
        }
        LOG(V2_INFO, "CDB (%i,%i) %i \t%s\n", l.size, l.lbd, slot.getNumActiveChunks(), out.c_str());
        i++;
        l.next(_max_lbd_partitioned_size);
    }
}*/

