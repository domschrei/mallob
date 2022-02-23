
#include "adaptive_clause_database.hpp"

AdaptiveClauseDatabase::AdaptiveClauseDatabase(int maxClauseSize, int maxLbdPartitionedSize, 
        int baseBufferSize, int numChunks, int numProducers, bool useChecksum):
    _max_lbd_partitioned_size(maxLbdPartitionedSize), 
    _chunk_size(baseBufferSize), 
    _use_checksum(useChecksum),
    _hist_deleted_in_slots(maxClauseSize) {

    // Initialize a clause slot for each length-LBD bucket
    BucketLabel l;
    while (l.size <= maxClauseSize) {

        int slotIdx = _slots.size();
        LOG(V5_DEBG, "SLOT (%i,%i)\n", l.size, l.lbd);
        _slots.push_back(BufferSlot(_chunk_size, l.size+(l.size>maxLbdPartitionedSize?1:0), l.size, l.lbd, numProducers));

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

        //slot.setDeletedClausesHistogram(_hist_deleted_in_slots);

        l.next(maxLbdPartitionedSize);
    }

    // Allocate free chunks
    for (int i = 0; i < numChunks; i++) {
        _free_chunks.push_back((uint8_t*) malloc(_chunk_size * sizeof(int)));
        atomics::incrementRelaxed(_num_free_chunks);
    }
}


bool AdaptiveClauseDatabase::addClause(int producerId, const Clause& c) {

    // Find correct clause slot, attempt to insert
    size_t slotIdx = getSlotIdx(c.size, c.lbd);
    if (slotIdx < 0 || slotIdx >= _slots.size()) {
        LOG(V1_WARN, "[WARN] %s -> invalid slot index %i\n", c.toStr().c_str(), slotIdx);
        return DROP; // clause is not acceptable
    }
    auto& slot = _slots[slotIdx];
    return slot.insert(c, producerId);
}

int AdaptiveClauseDatabase::bulkAddClauses(int producerId, const std::vector<Clause>& clauses, 
        SolvingStatistics& stats, std::function<bool(const Clause& c)> conditional) {

    int numAdded = 0;

    // As long as there are clauses left:
    const Clause* cPtr = clauses.data();
    BucketLabel l;
    int slotIdx = 0;
    while (cPtr != clauses.data()+clauses.size()) {
        auto& clause = *cPtr;
        assert(clause.begin != nullptr);

        // Find correct slot for the clause
        while (l.size != clause.size || (l.size <= _max_lbd_partitioned_size && l.lbd != clause.lbd)) {
            l.next(_max_lbd_partitioned_size);
            slotIdx++;
        }
        if (slotIdx < 0 || slotIdx >= _slots.size() || !conditional(clause)) {
            cPtr++;
            continue;
        }

        // Insert as many clauses as you can, setting cPtr to the first clause *not* added
        bool success = _slots[slotIdx].insert(clause, producerId);
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

std::vector<int> AdaptiveClauseDatabase::exportBuffer(int sizeLimit, int& numExportedClauses, 
        const int minClauseLength, const int maxClauseLength, bool sortClauses) {

    int zero = 0;
    numExportedClauses = 0;
                
    std::vector<int> selection;
    if (sizeLimit > 0) selection.reserve(sizeLimit);
    size_t hash = 1;

    // Reserve space for checksum at the beginning of the buffer
    int numInts = (sizeof(size_t)/sizeof(int));
    for (int i = 0; i < numInts; i++) selection.push_back(0);

    BucketLabel bucket;
    int bufIdx = 0;
    int lastPushedCounterIdx = -1;
    while ((sizeLimit < 0 || selection.size() < sizeLimit) 
            && bufIdx < _slots.size()) {
        
        // Get size and LBD of current bucket
        int clauseSize = bucket.size;
        int clauseLbd = bucket.lbd;

        // Check that this bucket has the desired clause length
        if ((minClauseLength < 0 || clauseSize >= minClauseLength) 
            && (maxClauseLength < 0 || clauseSize <= maxClauseLength)) {

            // Counter integer for the clauses of this bucket
            int counterPos;
            bool partitionedByLbd = clauseSize <= _max_lbd_partitioned_size;

            // Fetch correct buffer list
            int effectiveClsLen = clauseSize + (partitionedByLbd ? 0 : 1);
            auto& buf = _slots[bufIdx];

            // Fetch as many clauses as available and as there is space
            if (sizeLimit < 0 || selection.size()+effectiveClsLen <= sizeLimit) {
                // Write next clauses
                
                // Initialize clause counter(s) as necessary
                while (lastPushedCounterIdx < bufIdx) {
                    selection.push_back(0);
                    counterPos = selection.size()-1;
                    lastPushedCounterIdx++;
                }
                
                // Fetch clauses
                size_t sizeBefore = selection.size();
                int numDesired = sizeLimit < 0 ? -1 : (sizeLimit - selection.size()) / effectiveClsLen;
                int received = buf.flush(numDesired, selection);
                assert(selection.size()-sizeBefore == received*effectiveClsLen 
                    || log_return_false("%i != %i!\n", selection.size()-sizeBefore, received*effectiveClsLen));
                
                // Update clause counter and stats
                selection[counterPos] += received;
                numExportedClauses += received;

                if (sortClauses) {
                    // Sort clauses alphanumerically
                    assert((selection.size()-sizeBefore) % effectiveClsLen == 0);
                    std::vector<int> clausesCopy(selection.data()+sizeBefore, selection.data()+selection.size());
                    std::vector<int*> clausePointers(received);
                    for (size_t i = 0; i < clausePointers.size(); i++) {
                        clausePointers[i] = clausesCopy.data()+i*effectiveClsLen;
                    }
                    std::sort(clausePointers.begin(), clausePointers.end(), 
                        InplaceClauseComparator(effectiveClsLen));
                    for (size_t i = 0; i < clausePointers.size(); i++) {
                        int* lits = clausePointers[i];
                        for (size_t x = 0; x < effectiveClsLen; x++) {
                            selection[sizeBefore+(effectiveClsLen*i)+x] = lits[x];
                        }
                    }
                }
                
                if (_use_checksum) {
                    for (size_t pos = sizeBefore; pos < selection.size(); pos += effectiveClsLen) {
                        hash_combine(hash, ClauseHasher::hash(
                            selection.data()+pos,
                            clauseSize+(partitionedByLbd ? 0 : 1), 3
                        ));
                    }
                }
            }
        }

        // Proceed to next bucket
        bucket.next(_max_lbd_partitioned_size);
        bufIdx++;
    }

    // Remove trailing zeroes
    size_t lastNonzeroIdx = selection.size()-1;
    while (lastNonzeroIdx > 0 && selection[lastNonzeroIdx] == 0) lastNonzeroIdx--;
    selection.resize(lastNonzeroIdx+1);

    // Write final hash checksum
    memcpy(selection.data(), &hash, sizeof(size_t));

    return selection;
}

BufferReader AdaptiveClauseDatabase::getBufferReader(int* begin, size_t size, bool useChecksums) {
    return BufferReader(begin, size, _max_lbd_partitioned_size, useChecksums);
}

BufferMerger AdaptiveClauseDatabase::getBufferMerger() {
    return BufferMerger(_max_lbd_partitioned_size, _use_checksum);
}

ClauseHistogram& AdaptiveClauseDatabase::getDeletedClausesHistogram() {
    return _hist_deleted_in_slots;
}

size_t AdaptiveClauseDatabase::getSlotIdx(int clauseSize, int lbd) {
    
    const int mlbdps = _max_lbd_partitioned_size;
    const int numPartitionedLengthsBefore = std::min(clauseSize-1, mlbdps);
    const int numPartitionedSlotsBefore = numPartitionedLengthsBefore == 0 ? 
        0 : 1 + numPartitionedLengthsBefore*(numPartitionedLengthsBefore-1) / 2;
    const int numNonpartitionedSlotsBefore = std::max(0, clauseSize - mlbdps - 1);
    const bool isLbdPartitioned = clauseSize <= mlbdps;
    
    const size_t index = numPartitionedSlotsBefore + numNonpartitionedSlotsBefore 
        + (isLbdPartitioned ? std::max(lbd, 2)-2 : 0);
    
    if (index < _slots.size()) return index;
    else return -1;
}
