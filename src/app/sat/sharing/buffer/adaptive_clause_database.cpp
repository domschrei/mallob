
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

    // Store initial literal budget in final (lowest priority) slot
    storeGlobalBudget(_total_literal_limit);
}

bool AdaptiveClauseDatabase::addClause(const Clause& c, bool sortLargeClause) {
    return addClause(c.begin, c.size, c.lbd, sortLargeClause);
}

bool AdaptiveClauseDatabase::addClause(int* cBegin, int cSize, int cLbd, bool sortLargeClause) {

    int len = cSize;
    auto [slotIdx, mode] = getSlotIdxAndMode(len, cLbd);

    // Try acquire budget through stealing from a less important slot
    bool acquired = false;
    int freed = tryAcquireBudget(slotIdx, len, /*freeingFactor=*/5);
    // Freed enough memory?
    if (freed >= len) {
        // Steal successful: use len freed lits for this clause implicitly
        acquired = true;
        // Release the freed budget which you DO NOT need for this clause insertion
        if (freed != len) storeLocalBudget(slotIdx, freed-len);
        
    } else if (freed > 0) {
        // Return insufficient freed budget to global storage
        storeLocalBudget(slotIdx, freed);
    }
    if (!acquired) return false;

    // Budget has been acquired successfully
    _nb_used_literals.fetch_add(len, std::memory_order_relaxed);
    
    if (cSize == 1) {
        _unit_slot.mtx->lock();
        _unit_slot.list.push_front(*cBegin);
        atomics::incrementRelaxed(_unit_slot.nbLiterals);
        assert_heavy(checkNbLiterals(_unit_slot));
        _unit_slot.mtx->unlock();
    } else if (cSize == 2) {
        _binary_slot.mtx->lock();
        _binary_slot.list.emplace_front(cBegin[0], cBegin[1]);
        atomics::addRelaxed(_binary_slot.nbLiterals, 2);
        assert_heavy(checkNbLiterals(_binary_slot));
        _binary_slot.mtx->unlock();
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
        slot.mtx->lock();
        slot.list.push_front(std::move(vec));
        atomics::addRelaxed(slot.nbLiterals, cSize);
        assert_heavy(checkNbLiterals(slot));
        slot.mtx->unlock();
    }

    return true;
}

int AdaptiveClauseDatabase::reserveLiteralBudget(int cSize, int cLbd) {
    
    auto [slotIdx, mode] = getSlotIdxAndMode(cSize, cLbd);
    int nbReserved = getLocalBudget(slotIdx);
    for (size_t i = slotIdx+1; i < _large_slots.size(); i++) {
        nbReserved += getLocalBudget(i);
        if (i == -1) {
            nbReserved += _binary_slot.nbLiterals.load(std::memory_order_relaxed);
        } else {
            nbReserved += _large_slots[i].nbLiterals.load(std::memory_order_relaxed);
        }
    }
    return nbReserved;
}

bool AdaptiveClauseDatabase::popFrontWeak(ExportMode mode, Mallob::Clause& out) {

    if (mode != ExportMode::NONUNITS) {
        if (popMallobClause(_unit_slot, /*giveUpOnLock=*/true, out)) return true;
    }
    if (mode == ExportMode::UNITS) return false;

    if (popMallobClause(_binary_slot, /*giveUpOnLock=*/true, out)) return true;

    for (int slotIdx = 0; slotIdx < _large_slots.size(); slotIdx++) {
        auto& slot = _large_slots[slotIdx];
        if (popMallobClause(slot, /*giveUpOnLock=*/true, out)) return true;
    }

    return false;
}

template <typename T>
bool AdaptiveClauseDatabase::popMallobClause(Slot<T>& slot, bool giveUpOnLock, Mallob::Clause& out) {
    if (slot.nbLiterals.load(std::memory_order_relaxed) == 0) return false;
    if (giveUpOnLock) {
        if (!slot.mtx->tryLock()) return false;
    } else {
        slot.mtx->lock();
    }
    if (slot.nbLiterals.load(std::memory_order_relaxed) == 0) {
        slot.mtx->unlock();
        return false;
    }
    assert(!slot.list.empty());
    int nbLiteralsBefore = slot.nbLiterals.load(std::memory_order_relaxed);
    T packed = std::move(slot.list.front());
    slot.list.pop_front();
    auto mc = getMallobClause(packed, slot.implicitLbdOrZero);

    storeGlobalBudget(mc.size);
    _nb_used_literals.fetch_sub(mc.size, std::memory_order_relaxed);
    atomics::subRelaxed(slot.nbLiterals, mc.size);

    assert_heavy(checkNbLiterals(slot, "popMallobClause(): " + mc.toStr() + "; " + std::to_string(nbLiteralsBefore) + " lits before"));
    slot.mtx->unlock();
    out = mc.copy(); // copy
    return true;
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
    
    if (slot.nbLiterals.load(std::memory_order_relaxed) == 0
        && slot.freeLocalBudget.load(std::memory_order_relaxed) == 0) 
        return;
    
    // Swap current clause information in the slot to another list
    std::forward_list<T> swappedSlot;
    int nbSwappedLits;
    int litsToStore = 0;
    {
        auto lock = slot.mtx->getLock();

        // Transfer local free budget to global budget, if necessary
        int freeBudget = slot.freeLocalBudget;
        if (freeBudget > 0) {
            slot.freeLocalBudget.store(0, std::memory_order_relaxed);
            litsToStore += freeBudget;
        }

        // Nothing to extract?
        if (slot.nbLiterals.load(std::memory_order_relaxed) == 0) {
            if (litsToStore > 0) storeGlobalBudget(litsToStore);
            return;
        }
        
        // Extract clauses
        swappedSlot.swap(slot.list);
        nbSwappedLits = slot.nbLiterals.load(std::memory_order_relaxed);
        slot.nbLiterals.store(0, std::memory_order_relaxed);
    }

    // Create clauses one by one
    std::vector<Mallob::Clause> flushedClauses;
    int remainingLits = builder.getMaxRemainingLits();
    int collectedLits = 0;
    auto itBefore = swappedSlot.before_begin();
    auto it = swappedSlot.begin();
    for (; it != swappedSlot.end(); ++it, ++itBefore) {
        T& elem = *it;
        Mallob::Clause clause = getMallobClause(elem, slot.implicitLbdOrZero);
        if (clause.size > remainingLits) break;

        // insert clause to export buffer
        remainingLits -= clause.size;
        collectedLits += clause.size;
        flushedClauses.push_back(std::move(clause));
    }

    // Return budget of extracted literals
    litsToStore += collectedLits;
    storeGlobalBudget(litsToStore);
    _nb_used_literals.fetch_sub(collectedLits, std::memory_order_relaxed);

    if (it != swappedSlot.end()) {
        // Re-insert swapped clauses which remained unused
        auto lock = slot.mtx->getLock();
        slot.list.splice_after(slot.list.before_begin(), swappedSlot, itBefore, swappedSlot.end());
        atomics::addRelaxed(slot.nbLiterals, nbSwappedLits - collectedLits);
        assert_heavy(checkNbLiterals(slot));
    } else {
        assert(nbSwappedLits == collectedLits || 
            log_return_false("[ERROR] slot advertised %i lits, collected %i lits\n", 
            nbSwappedLits, collectedLits));
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

    /*
    std::string out = "lim=" + std::to_string(totalLiteralLimit) + " FREE LOCAL BUDGETS: ";
    out += std::to_string(_unit_slot.freeLocalBudget.load()) + " ";
    out += std::to_string(_binary_slot.freeLocalBudget.load()) + " ";
    for (auto& slot : _large_slots) 
        out += std::to_string(slot.freeLocalBudget.load()) + " ";
    LOG(V2_INFO, "%s\n", out.c_str());
    */

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

int AdaptiveClauseDatabase::tryAcquireBudget(int callingSlot, int numDesired, float freeingFactor) {

    int numFreed = 0;
    numDesired *= freeingFactor;
    /*
    // First fetch up to numDesired free literals from global budget, use to initialize numFreed
    int freeLits = _num_free_literals.load(std::memory_order_relaxed);
    while (freeLits > 0 && !_num_free_literals.compare_exchange_strong(freeLits, std::max(0, freeLits-numDesired))) {}
    assert(freeLits >= 0);
    if (freeLits > 0) {
        // successful
        numFreed = std::min(freeLits, numDesired); // at most #desired, at most num. freed
        if (numFreed == numDesired) return numDesired;
    }
    */

   // First of all, try to get budget from this slot's "internal budget"
   {
       if (callingSlot == -2) numFreed += stealBudgetFromSlot(_unit_slot, numDesired, /*dropClauses=*/false);
       else if (callingSlot == -1) numFreed += stealBudgetFromSlot(_binary_slot, numDesired, /*dropClauses=*/false);
       else numFreed += stealBudgetFromSlot(_large_slots[callingSlot], numDesired, /*dropClauses=*/false);
   }
   if (numFreed >= numDesired) return numFreed;

    // Steal from a less important slot
    int lb = std::max(-1, callingSlot);
    int ub = _large_slots.size()-1;
    for (int slotIdx = ub; numFreed < numDesired && slotIdx > lb; slotIdx--) {
        auto& slot = _large_slots[slotIdx];
        numFreed += stealBudgetFromSlot(slot, numDesired-numFreed, /*dropClauses=*/true);
        //if (numLiterals > 0) LOG(V2_INFO, "%i -> %i : Freed %i lits\n", callingSlot, slotIdx, numLiterals);
    }
    if (numFreed < numDesired && callingSlot == -2) {
        numFreed += stealBudgetFromSlot(_binary_slot, numDesired-numFreed, /*dropClauses=*/true);
        //if (numLiterals > 0) LOG(V2_INFO, "%i -> %i : Freed %i lits\n", callingSlot, -2, numLiterals);
    }

    return numFreed;
}

template <typename T>
int AdaptiveClauseDatabase::stealBudgetFromSlot(Slot<T>& slot, int desiredLiterals, bool dropClauses) {
    
    if (slot.nbLiterals.load(std::memory_order_relaxed) == 0
        && slot.freeLocalBudget.load(std::memory_order_relaxed) == 0) 
        return 0;

    auto lock = slot.mtx->getLock();
    assert_heavy(checkNbLiterals(slot, "before dropClauses()"));
    int nbLiteralsBefore = slot.nbLiterals.load(std::memory_order_relaxed);

    int freeBudget = std::min(slot.freeLocalBudget.load(std::memory_order_relaxed), desiredLiterals);
    int nbCollectedLits = 0;
    int nbCollectedClauses = 0;

    auto it = slot.list.begin();
    if (dropClauses) {
        while (freeBudget + nbCollectedLits < desiredLiterals && it != slot.list.end()) {
            if constexpr (std::is_same<int, T>::value) {
                ++nbCollectedLits;
                _hist_deleted_in_slots.increment(1);
            }
            if constexpr (std::is_same<std::pair<int, int>, T>::value) {
                nbCollectedLits += 2;
                _hist_deleted_in_slots.increment(2);
            }
            if constexpr (std::is_same<std::vector<int>, T>::value) {
                int clslen = it->size() - (slot.implicitLbdOrZero==0 ? 1 : 0);
                nbCollectedLits += clslen;
                _hist_deleted_in_slots.increment(clslen);
            }
            nbCollectedClauses++;
            ++it;
        }
    }

    if (freeBudget+nbCollectedLits == 0) {
        return 0;
    }

    // Extract part of the list
    std::forward_list<T> swappedList;
    swappedList.splice_after(swappedList.before_begin(), slot.list, slot.list.before_begin(), it);
    
    atomics::subRelaxed(slot.nbLiterals, nbCollectedLits);
    atomics::subRelaxed(slot.freeLocalBudget, freeBudget);
    atomics::subRelaxed(_nb_used_literals, nbCollectedLits);

    assert_heavy(checkNbLiterals(slot, "dropClauses(): collected " 
        + std::to_string(nbCollectedLits) + " literals from " 
        + std::to_string(nbCollectedClauses) + " clauses; " 
        + std::to_string(nbLiteralsBefore) + " lits before"));
    
    // Elements are cleaned up automatically
    return freeBudget + nbCollectedLits;
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

