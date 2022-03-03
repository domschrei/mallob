
#include "adaptive_clause_database.hpp"

AdaptiveClauseDatabase::AdaptiveClauseDatabase(Setup setup):
    _max_lbd_partitioned_size(setup.maxLbdPartitionedSize),
    _max_clause_length(setup.maxClauseLength),
    _slots_for_sum_of_length_and_lbd(setup.slotsForSumOfLengthAndLbd),
    _chunk_size(setup.chunkSize), 
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
            BufferChunk::Mode opMode;

            // Decide what kind of slot we need for this combination
            int sumOfLengthAndLbd = clauseLength+lbd;
            if (setup.slotsForSumOfLengthAndLbd && sumOfLengthAndLbd >= 6 
                    && sumOfLengthAndLbd <= maxSumOfLengthAndLbd) {
                // Shared slot for all clauses of this length+lbd sum
                opMode = BufferChunk::SAME_SUM;
                representantKey = std::pair<int, int>(sumOfLengthAndLbd-2, 2);
            } else if (clauseLength > setup.maxLbdPartitionedSize) {
                // Slot for all clauses of this length
                opMode = BufferChunk::SAME_SIZE;
                representantKey = std::pair<int, int>(clauseLength, clauseLength);
            } else {
                // Exclusive slot for this length-lbd combination
                opMode = BufferChunk::SAME_SIZE_AND_LBD;
                representantKey = lengthLbdPair;
            }

            if (!_size_lbd_to_slot_idx.count(representantKey)) {
                // Create new slot

                int slotIdx = _slots.size();
                _slots.push_back(BufferSlot(_chunk_size, opMode, 
                    opMode==BufferChunk::SAME_SUM ? sumOfLengthAndLbd : clauseLength, 
                    setup.numProducers));
                _slot_lbd.push_back(opMode == BufferChunk::SAME_SIZE_AND_LBD ? representantKey.second : -1);
                _size_lbd_to_slot_idx[representantKey] = slotIdx;

                _slots.back().setChunkSource([&, slotIdx]() {
                    if (_num_free_chunks.load(std::memory_order_relaxed) > 0) {
                        auto lock = _free_chunks_mutex.getLock();
                        if (!_free_chunks.empty()) {
                            auto memory = _free_chunks.front();
                            _free_chunks.pop_front();
                            atomics::decrementRelaxed(_num_free_chunks);
                            return memory;
                        } 
                    }
                    for (size_t i = _slots.size()-1; i > slotIdx; i--) {
                        uint8_t* memory = _slots[i].tryStealChunk();
                        if (memory != nullptr) return memory;
                    }
                    return (uint8_t*)nullptr;
                });

                _slots.back().setChunkSink([&](uint8_t* data) {
                    auto lock = _free_chunks_mutex.getLock();
                    _free_chunks.push_back(data);
                    atomics::incrementRelaxed(_num_free_chunks);
                });

                _slots.back().setDeletedClausesHistogram(_hist_deleted_in_slots);
            }

            // May be a no-op
            _size_lbd_to_slot_idx[lengthLbdPair] = _size_lbd_to_slot_idx[representantKey];
            //LOG(V2_INFO, "SLOT (%i,%i) ~> %i\n", lengthLbdPair.first, lengthLbdPair.second, _size_lbd_to_slot_idx[representantKey]);
        }
    }

    // Allocate free chunks
    for (int i = 0; i < setup.numChunks; i++) {
        _free_chunks.push_back((uint8_t*) malloc(_chunk_size * sizeof(int)));
        atomics::incrementRelaxed(_num_free_chunks);
    }
}


bool AdaptiveClauseDatabase::addClause(int producerId, const Clause& c) {

    Clause _clause = c;
    if (_clause.lbd < 2 && _clause.size > 1) {
        //LOG(V1_WARN, "Length-LBD combination (%i,%i) learned!\n", _clause.size, _clause.lbd);
        _clause.lbd = 2;
    }

    // Find correct clause slot, attempt to insert
    size_t slotIdx = getSlotIdx(_clause.size, _clause.lbd);
    //LOG(V2_INFO, "(%i,%i) ~> %i\n", c.size, c.lbd, slotIdx);
    if (slotIdx < 0 || slotIdx >= _slots.size()) {
        LOG(V1_WARN, "[WARN] %s -> invalid slot index %i\n", _clause.toStr().c_str(), slotIdx);
        return false; // clause is not acceptable
    }
    auto& slot = _slots[slotIdx];
    return slot.insert(_clause, producerId);
}

int AdaptiveClauseDatabase::bulkAddClauses(int producerId, const std::vector<Clause>& clauses, 
        std::function<bool(const Clause& c)> conditional) {

    int numAdded = 0;

    std::pair<int, int> slotKey;
    int slotIdx = -1;
    // As long as there are clauses left:
    for (auto& c : clauses) {
        Clause _clause = c;
        if (_clause.lbd < 2 && _clause.size > 1) {
            //LOG(V1_WARN, "Length-LBD combination (%i,%i) learned!\n", _clause.size, _clause.lbd);
            _clause.lbd = 2;
        }
        assert(_clause.begin != nullptr);
        assert(_clause.lbd <= _clause.size);

        std::pair<int, int> lengthLbdPair(_clause.size, _clause.lbd);
        if (lengthLbdPair != slotKey) {
            slotKey = lengthLbdPair;
            slotIdx = getSlotIdx(_clause.size, _clause.lbd);
        }

        if (!conditional(_clause)) continue;

        // Insert
        //LOG(V2_INFO, "INSERT %s ~> %i\n", _clause.toStr().c_str(), slotIdx);
        bool success = _slots[slotIdx].insert(_clause, producerId);
        if (success) numAdded++;
    }

    return numAdded;
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

std::vector<int> AdaptiveClauseDatabase::exportBuffer(int totalLiteralLimit, int& numExportedClauses, 
        ExportMode mode, bool sortClauses) {

    numExportedClauses = 0;
    int numAddedLits = 0;
                
    BufferBuilder builder(totalLiteralLimit, _max_clause_length, _slots_for_sum_of_length_and_lbd);
    BufferIterator it(_max_clause_length, _slots_for_sum_of_length_and_lbd);

    for (int slotIdx = 0; slotIdx < _slots.size(); slotIdx++) {
        auto& slot = _slots[slotIdx];

        int modeParam = slot.getModeParam();
        if (mode == ExportMode::NONUNITS && modeParam == 1) continue;
        if (mode == ExportMode::UNITS && modeParam != 1) break;

        auto opMode = slot.getOperationMode();
        bool differentLbdValues = opMode != BufferChunk::SAME_SIZE_AND_LBD;

        // Fetch clauses
        std::vector<int> exportedClauseInts;
        int numDesiredLits = totalLiteralLimit == -1 ? -1 : totalLiteralLimit - numAddedLits;
        int receivedClauses = slot.flush(numDesiredLits, exportedClauseInts);

        //std::string str;
        //for (int lit : exportedClauseInts) str += std::to_string(lit) + " ";
        //LOG(V2_INFO, "EXP %s\n", str.c_str());
        
        if (differentLbdValues || sortClauses) {
            // Sort clauses alphanumerically.

            std::vector<int*> clausePointers(receivedClauses);
            size_t pos = 0;
            for (size_t i = 0; i < clausePointers.size(); i++) {
                clausePointers[i] = exportedClauseInts.data()+pos;
                int clauseLength = slot.getEffectiveExportedClauseLength(exportedClauseInts[pos]);
                assert(clauseLength > 0);
                assert(clauseLength <= 255);
                pos += clauseLength;
            }

            // Sort
            if (opMode == BufferChunk::SAME_SUM) {
                std::sort(clausePointers.begin(), clausePointers.end(), 
                    InplaceClauseComparatorUniformSizeLbdSum(modeParam));
            } else {
                std::sort(clausePointers.begin(), clausePointers.end(), 
                    InplaceClauseComparatorUniformSize(
                        opMode == BufferChunk::SAME_SIZE_AND_LBD ? modeParam : modeParam+1
                    )
                );
            }

            // Insert clauses according to sorted pointers
            for (size_t i = 0; i < clausePointers.size(); i++) {
                
                int* lits = clausePointers[i];
                assert(!differentLbdValues || (lits[0] > 0 && lits[0] <= _max_clause_length));
                int effectiveLength = slot.getEffectiveExportedClauseLength(lits[0]);
                int trueLength = slot.getTrueExportedClauseLength(lits[0]);
                assert(effectiveLength - trueLength <= 1);
                int lbd = differentLbdValues ? lits[0] : _slot_lbd[slotIdx];
                assert(lbd > 0 && lbd <= trueLength);
                Clause c {lits+effectiveLength-trueLength, trueLength, lbd};

                while (it.clauseLength != trueLength || it.lbd != lbd) {
                    //LOG(V2_INFO, "%s GROUP (%i,%i)\n", c.toStr().c_str(), it.clauseLength, it.lbd);
                    it.nextLengthLbdGroup();
                    assert(it.clauseLength < 256);
                }

                numAddedLits += c.size;
                assert(totalLiteralLimit == -1 || numAddedLits <= totalLiteralLimit);
                bool success = builder.append(c);
                assert(success);
                numExportedClauses++;
            }

        } else if (!exportedClauseInts.empty()) {

            // Insert clauses one by one
            Clause c {nullptr, modeParam, _slot_lbd[slotIdx]};
            assert(c.lbd > 0);
            while (it.clauseLength != c.size || it.lbd != c.lbd) {
                //LOG(V2_INFO, "len=%i lbd=%i: GROUP (%i,%i)\n", c.size, c.lbd, it.clauseLength, it.lbd);
                it.nextLengthLbdGroup();
                assert(it.clauseLength < 256);
            }
            for (size_t i = 0; i+c.size <= exportedClauseInts.size(); i += c.size) {
                numAddedLits += c.size;
                assert(totalLiteralLimit == -1 || numAddedLits <= totalLiteralLimit);
                c.begin = exportedClauseInts.data() + i;
                bool success = builder.append(c);
                assert(success);
                numExportedClauses++;
            }
        }
    }

    return builder.extractBuffer();
}

BufferReader AdaptiveClauseDatabase::getBufferReader(int* begin, size_t size, bool useChecksums) {
    return BufferReader(begin, size, _max_clause_length, _slots_for_sum_of_length_and_lbd, useChecksums);
}

BufferMerger AdaptiveClauseDatabase::getBufferMerger(int sizeLimit) {
    return BufferMerger(sizeLimit, _max_clause_length, _slots_for_sum_of_length_and_lbd, _use_checksum);
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
