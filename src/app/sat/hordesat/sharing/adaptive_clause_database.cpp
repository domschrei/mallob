
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

            if (!_size_lbd_to_slot_idx.count(representantKey)) {
                // Create new slot
                int slotIdx;
                if (clauseLength == 1) {
                    slotIdx = -2;
                    _unit_slot = (
                        new ClauseSlotSet<ProducedUnitClause>(
                            slotIdx, opMode, _num_free_literals, _max_nonempty_slot, 
                            [&]() {return freeLowPriorityLiterals(slotIdx);}
                        )
                    );
                } else if (clauseLength == 2) {
                    slotIdx = -1;
                    _binary_slot = (
                        new ClauseSlotSet<ProducedBinaryClause>(
                            slotIdx, opMode, _num_free_literals, _max_nonempty_slot, 
                            [&]() {return freeLowPriorityLiterals(slotIdx);}
                        )
                    );
                } else {
                    slotIdx = _large_slots.size();
                    _large_slots.push_back(
                        new ClauseSlotSet<ProducedLargeClause>(
                            slotIdx, opMode, _num_free_literals, _max_nonempty_slot, 
                            [&, slotIdx]() {return freeLowPriorityLiterals(slotIdx);}
                        )
                    );
                }
                _size_lbd_to_slot_idx[representantKey] = slotIdx;
            }

            // May be a no-op
            _size_lbd_to_slot_idx[lengthLbdPair] = _size_lbd_to_slot_idx[representantKey];
            //LOG(V2_INFO, "SLOT (%i,%i) ~> %i\n", lengthLbdPair.first, lengthLbdPair.second, _size_lbd_to_slot_idx[representantKey]);
        }
    }

    int maxSlotIdx = _large_slots.size()-1;
    _unit_slot->setMaxSlotIdx(maxSlotIdx);
    _binary_slot->setMaxSlotIdx(maxSlotIdx);
    for (auto& slot : _large_slots) slot->setMaxSlotIdx(maxSlotIdx);

    // Indicate initial literal budget
    _num_free_literals.store(setup.numLiterals, std::memory_order_relaxed);
    _max_nonempty_slot.store(maxSlotIdx, std::memory_order_relaxed);
}

ClauseSlotInsertResult AdaptiveClauseDatabase::addClause(int producerId, const Clause& c) {

    ClauseSlotInsertResult result;
    if (c.size == 1) {
        result = _unit_slot->insert(c, producerId);
    } else if (c.size == 2) {
        result = _binary_slot->insert(c, producerId);
    } else {
        int slotIdx = getSlotIdx(c.size, c.lbd);
        result = _large_slots.at(slotIdx)->insert(c, producerId);
    }
    return result;
}

Mallob::Clause AdaptiveClauseDatabase::popFront(ExportMode mode) {

    if (mode != ExportMode::NONUNITS) {
        auto cls = _unit_slot->pop();
        if (cls.valid()) return prod_cls::toMallobClause(std::move(cls));
    }
    if (mode == ExportMode::UNITS) return Mallob::Clause();

    auto cls = _binary_slot->pop();
    if (cls.valid()) return prod_cls::toMallobClause(std::move(cls));

    for (int slotIdx = 0; slotIdx < _large_slots.size(); slotIdx++) {
        auto& slot = *_large_slots[slotIdx];
        auto cls = slot.pop();
        if (cls.valid()) return prod_cls::toMallobClause(std::move(cls));
    }

    return Mallob::Clause();
}

template <typename T> 
void AdaptiveClauseDatabase::handleFlushedClauses(std::vector<T>& flushedClauses, 
    ClauseSlotMode slotMode, bool sortClauses, BufferBuilder& builder) {
    
    bool differentLbdValues = slotMode != ClauseSlotMode::SAME_SIZE_AND_LBD;
    if (differentLbdValues || sortClauses) {
        // Sort
        std::sort(flushedClauses.begin(), flushedClauses.end());
    }

    // Append clauses to buffer builder
    for (auto& producedClause : flushedClauses) {
        _last_exported_buffer_producers.push_back(producedClause.producers);
        Clause c = prod_cls::toMallobClause(producedClause);
        bool success = builder.append(c);
        assert(success);
        //log(V2_INFO, "%i : EXPORTED %s\n", producedClause.producers, c.toStr().c_str());
    }
}

const std::vector<int>& AdaptiveClauseDatabase::exportBuffer(int totalLiteralLimit, int& numExportedClauses, 
        ExportMode mode, bool sortClauses) {

    _last_exported_buffer.clear();
    _last_exported_buffer_producers.clear();

    BufferBuilder builder(totalLiteralLimit, _max_clause_length, _slots_for_sum_of_length_and_lbd,
        &_last_exported_buffer);

    if (mode != ExportMode::NONUNITS) {
        // Export unit clauses.
        std::vector<ProducedUnitClause> flushedClauses = _unit_slot->flush(builder.getMaxRemainingLits());
        handleFlushedClauses(flushedClauses, _unit_slot->getSlotMode(), sortClauses, builder);
    }
    if (mode != ExportMode::UNITS) {
        // Export all other clauses.

        // Binary clauses first.
        std::vector<ProducedBinaryClause> flushedBClauses = _binary_slot->flush(builder.getMaxRemainingLits());
        handleFlushedClauses(flushedBClauses, _binary_slot->getSlotMode(), sortClauses, builder);

        // All other clauses.
        std::vector<ProducedLargeClause> flushedClauses;
        for (int slotIdx = 0; slotIdx < _large_slots.size(); slotIdx++) {
            auto& slot = *_large_slots[slotIdx];
            flushedClauses = slot.flush(builder.getMaxRemainingLits());
            handleFlushedClauses(flushedClauses, slot.getSlotMode(), sortClauses, builder);
        }
    }

    numExportedClauses = builder.getNumAddedClauses();
    return _last_exported_buffer;
}

int AdaptiveClauseDatabase::freeLowPriorityLiterals(int callingSlot) {
    int numFreed = 0;
    int numDesired = 5*_max_clause_length;
    int lb = std::max(-1, callingSlot);
    int ub = _max_nonempty_slot.load(std::memory_order_relaxed);

    for (int slotIdx = ub; numFreed < _max_clause_length && slotIdx > lb; slotIdx--) {
        int numLiterals = _large_slots[slotIdx]->free(numDesired);
        //if (numLiterals > 0) LOG(V2_INFO, "%i -> %i : Freed %i lits\n", callingSlot, slotIdx, numLiterals);
        numDesired -= numLiterals;
        numFreed += numLiterals;
    }

    if (numFreed < _max_clause_length && callingSlot == -2) {
        int numLiterals = _binary_slot->free(numDesired);
        //if (numLiterals > 0) LOG(V2_INFO, "%i -> %i : Freed %i lits\n", callingSlot, -2, numLiterals);
        numFreed += numLiterals;
    }

    int newUb = _max_nonempty_slot.load(std::memory_order_relaxed);
    //if (ub != newUb) LOG(V2_INFO, "FREELOWPRIO %i ~> %i\n", ub, newUb);
    return numFreed;
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

BufferReader AdaptiveClauseDatabase::getReaderForLastExportedBuffer() {
    return BufferReader(_last_exported_buffer.data(), _last_exported_buffer.size(), _max_clause_length, _slots_for_sum_of_length_and_lbd, _use_checksum);
}

ClauseHistogram& AdaptiveClauseDatabase::getDeletedClausesHistogram() {
    return _hist_deleted_in_slots;
}

size_t AdaptiveClauseDatabase::getSlotIdx(int clauseSize, int lbd) {
    assert(lbd >= 1);
    assert(clauseSize == 1 || lbd >= 2 || log_return_false("(%i,%i) invalid length-clause combination!\n", clauseSize, lbd));
    assert(lbd <= clauseSize);
    auto pair = std::pair<int, int>(clauseSize, lbd);
    assert(_size_lbd_to_slot_idx.count(pair) 
        || log_return_false("(%i,%i) not a valid slot!\n", pair.first, pair.second));
    return _size_lbd_to_slot_idx.at(pair);
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

