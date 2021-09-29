
#ifndef DOMPASCH_MALLOB_WAITFREE_CLAUSE_DATABASE_HPP
#define DOMPASCH_MALLOB_WAITFREE_CLAUSE_DATABASE_HPP

#include <list>

#include "clause_slot.hpp"
#include "chunk_manager.hpp"
#include "bucket_label.hpp"
#include "buffer_reader.hpp"
#include "buffer_merger.hpp"
#include "util/periodic_event.hpp"
#include "app/sat/hordesat/solvers/solver_statistics.hpp"

class AdaptiveClauseDatabase {

private:
    ChunkManager _chunk_mgr;
    std::vector<ClauseSlot> _slots;

    int _max_lbd_partitioned_size;
    int _chunk_size;
    bool _use_checksum;

    ClauseHistogram _hist_deleted_in_slots;

public: 
    AdaptiveClauseDatabase(int maxClauseSize, int maxLbdPartitionedSize, int baseBufferSize, int numChunks, int numProducers, bool useChecksum = false):
            _max_lbd_partitioned_size(maxLbdPartitionedSize), _chunk_size(baseBufferSize), _use_checksum(useChecksum),
            _hist_deleted_in_slots(maxClauseSize) {
        
        _chunk_mgr = ChunkManager(numChunks, baseBufferSize);
        
        BucketLabel l;
        while (l.size <= maxClauseSize) {

            _slots.push_back(ClauseSlot(l.size, l.lbd, l.size <= maxLbdPartitionedSize, _chunk_size, numProducers));
            int slotIdx = _slots.size()-1;

            _slots.back().setChunkSink([&](int* data) {
                _chunk_mgr.insertChunk(data);
            });

            _slots.back().setChunkSource([this, slotIdx]() {
                std::pair<ClauseSlot::SlotResult, int*> pair {ClauseSlot::TOTAL_FAIL, nullptr};

                int* chunk = _chunk_mgr.getChunkOrNullptr();
                if (chunk == nullptr) {
                    // No chunks available right now.
                    // Try to steal chunk from slot to the right.
                    for (size_t i = _slots.size()-1; i > slotIdx; i--) {
                        auto result = _slots[i].releaseChunk(chunk);
                        // Try prev. slot on total fail; stop otherwise
                        if (result != ClauseSlot::TOTAL_FAIL) {
                            pair.first = result;
                            break;
                        }
                    }
                } else pair.first = ClauseSlot::SUCCESS;

                pair.second = chunk;
                return pair;
            });

            _slots.back().setDeletedClausesHistogram(_hist_deleted_in_slots);

            l.next(maxLbdPartitionedSize);
        }
    }

    ~AdaptiveClauseDatabase() = default;

    enum AddClauseResult {SUCCESS, TRY_LATER, DROP};
    AddClauseResult addClause(int producerId, const Clause& c) {

        // Find correct clause slot, attempt to insert
        size_t slotIdx = getSlotIdx(c.size, c.lbd);
        if (slotIdx < 0 || slotIdx >= _slots.size()) {
            log(V1_WARN, "[WARN] %s -> invalid slot index %i\n", c.toStr().c_str(), slotIdx);
            return DROP; // clause is not acceptable
        }
        auto& slot = _slots[slotIdx];
        auto result = slot.insert(producerId, c);
        if (result == ClauseSlot::SlotResult::SUCCESS) return SUCCESS;
        // Fail spuriously if a resource inside was busy
        if (result == ClauseSlot::SlotResult::SPURIOUS_FAIL) return TRY_LATER;
        return DROP; // total failure
    }

    void bulkAddClauses(int producerId, const std::vector<Clause>& clauses, std::list<Clause>& deferredOut, SolvingStatistics& stats,
            std::function<bool(const Clause& c)> conditional = [](const Clause&) {return true;}) {

        const Clause* cPtr = clauses.data();
        while (cPtr != clauses.data()+clauses.size()) {
            auto& clause = *cPtr;
            assert(clause.begin != nullptr);
            size_t slotIdx = getSlotIdx(clause.size, clause.lbd);
            if (slotIdx < 0 || slotIdx >= _slots.size()) {
                cPtr++;
                continue;
            }
            // Insert as many clauses as you can, setting cPtr to the first clause *not* added
            _slots[slotIdx].insert(producerId, cPtr, clauses.data()+clauses.size(), deferredOut, stats, conditional);
            //log(V2_INFO, "BULKADD (%p,%p) -> %p\n", clauses.data(), clauses.data()+clauses.size(), cPtr);
        }
    }

    void printChunks(int nextExportSize = -1) {
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
            log(V2_INFO, "CDB (%i,%i) %i \t%s\n", l.size, l.lbd, slot.getNumActiveChunks(), out.c_str());
            i++;
            l.next(_max_lbd_partitioned_size);
        }
    }
    
    std::vector<int> exportBuffer(int sizeLimit, int& numExportedClauses, 
            const int minClauseLength = -1, const int maxClauseLength = -1) {

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
        while ((sizeLimit < 0 || selection.size() < sizeLimit) 
                && bufIdx < _slots.size()) {
            
            // Get size and LBD of current bucket
            int clauseSize = bucket.size;
            int clauseLbd = bucket.lbd;
            
            // The counter for the clauses of this bucket goes here
            selection.push_back(0);

            // Check that this bucket has the desired clause length
            if ((minClauseLength < 0 || clauseSize >= minClauseLength) 
                && (maxClauseLength < 0 || clauseSize <= maxClauseLength)) {

                // Counter integer for the clauses of this bucket
                int counterPos = selection.size()-1;
                bool partitionedByLbd = clauseSize <= _max_lbd_partitioned_size;

                // Fetch correct buffer list
                int effectiveClsLen = clauseSize + (partitionedByLbd ? 0 : 1);
                auto& buf = _slots[bufIdx];

                // Fetch as many clauses as available and as there is space
                if (sizeLimit < 0 || selection.size()+effectiveClsLen <= sizeLimit) {
                    // Write next clauses
                    size_t sizeBefore = selection.size();
                    int numDesired = sizeLimit < 0 ? -1 : (sizeLimit - selection.size()) / effectiveClsLen;
                    int received = buf.getClauses(selection, numDesired);
                    selection[counterPos] += received;
                    numExportedClauses += received;
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

        // Also iterate over all subsequent clause slots to recognize unused / stale chunks
        while (bufIdx < _slots.size()) {
            _slots[bufIdx].getClauses(selection, /*maxNumClauses=*/0);
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

    BufferReader getBufferReader(int* begin, size_t size, bool useChecksums = false) {
        return BufferReader(begin, size, _max_lbd_partitioned_size, useChecksums);
    }

    BufferMerger getBufferMerger() {
        return BufferMerger(_max_lbd_partitioned_size, _use_checksum);
    }

    ClauseHistogram& getDeletedClausesHistogram() {
        return _hist_deleted_in_slots;
    }

private:
    size_t getSlotIdx(int clauseSize, int lbd) {
        
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

};

#endif
