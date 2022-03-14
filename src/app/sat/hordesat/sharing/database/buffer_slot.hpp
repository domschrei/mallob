
#pragma once

#include <list>
#include <functional>

#include "buffer_chunk.hpp"
#include "app/sat/hordesat/utilities/clause_filter.hpp"
#include "app/sat/hordesat/sharing/clause_histogram.hpp"

class BufferSlot {

private:
    int _chunk_size;
    int _num_producers;

    BufferChunk _working_chunk;
    std::atomic_int _num_full_chunks {0};
    Mutex _full_chunks_mutex;
    std::list<std::pair<size_t, uint8_t*>> _full_chunks;

    std::atomic_bool _replacing_chunk {false};
    std::function<uint8_t*()> _chunk_source;
    std::function<void(uint8_t*)> _chunk_sink;
    std::function<void(Clause&)> _clause_deleter;

    ClauseHistogram* _hist_deleted_in_slots = nullptr;

public:
    BufferSlot() {}
    BufferSlot(int chunkSize, BufferChunk::Mode mode, int modeParam, int numProducers) : 
        _chunk_size(chunkSize), _num_producers(numProducers), 
        _working_chunk(chunkSize, mode, modeParam, numProducers) {}

    BufferSlot(BufferSlot&& moved) :
        _chunk_size(moved._chunk_size), _num_producers(moved._num_producers),
        _working_chunk(std::move(moved._working_chunk)), 
        _num_full_chunks(moved._num_full_chunks.load()), _full_chunks(moved._full_chunks),
        _chunk_source(moved._chunk_source), _chunk_sink(moved._chunk_sink) {}

    void setChunkSource(std::function<uint8_t*()> chunkSource) {_chunk_source = chunkSource;}
    void setChunkSink(std::function<void(uint8_t*)> chunkSink) {_chunk_sink = chunkSink;}

    bool insert(const Mallob::Clause& c, int producerId) {

        // Attempt to add the clause
        bool success = _working_chunk.tryInsert(c.begin, producerId, c.lbd);
        while (!success) {

            // Not successful: chunk is (or was) full.
            // Try to replace it with a new chunk yourself:
            bool replacingChunk = false;
            bool triedToAcquireChunk = false;
            if (_replacing_chunk.compare_exchange_strong(replacingChunk, true, std::memory_order_acquire)) {
                // Acquired exclusive right to replace chunk

                // Fetch a chunk
                uint8_t* flushedMemory = nullptr;
                size_t numFlushedBytes;
                {
                    flushedMemory = _chunk_source();
                    triedToAcquireChunk = true;
                    if (flushedMemory != nullptr) {
                        // Flush the working chunk's buffer into the acquired chunk
                        numFlushedBytes = _working_chunk.flushBuffer(flushedMemory);
                    }
                }

                // Release right to replace chunk
                replacingChunk = true;
                _replacing_chunk.compare_exchange_strong(replacingChunk, false, std::memory_order_release);
                
                LOG(V5_DEBG, "(%i,%i) FETCH CHUNK success=%i\n", 
                    c.size, c.lbd, flushedMemory == nullptr ? 0 : 1);
                
                // Move the extracted full buffer to the list of full chunks
                if (flushedMemory != nullptr) {
                    auto lock = _full_chunks_mutex.getLock();
                    _full_chunks.push_back(std::pair<size_t, uint8_t*>(numFlushedBytes, flushedMemory));
                    _num_full_chunks.fetch_add(1, std::memory_order_acq_rel);
                }
            }

            // Retry insertion
            success = _working_chunk.tryInsert(c.begin, producerId, c.lbd);
            
            // No success at retry AND personally attempted to acquire a new chunk? Return failure.
            if (!success && triedToAcquireChunk) {
                return false;
            }
        }
        return true;
    }

    int flush(int totalLiteralLimit, std::vector<int>& out) {

        int read = 0;
        int numFullChunks = _num_full_chunks.load(std::memory_order_relaxed);

        // Loop over all available full chunks as long as remaining size permits
        while ((totalLiteralLimit == -1 || totalLiteralLimit > 0) 
                && _num_full_chunks.load(std::memory_order_relaxed) > 0) {
            
            // Try to fetch a full chunk
            size_t numBytesInMem = 0;
            int* mem = nullptr;
            {
                auto lock = _full_chunks_mutex.getLock();
                if (!_full_chunks.empty()) {
                    numBytesInMem = (int)_full_chunks.front().first;
                    mem = (int*)_full_chunks.front().second;
                    _full_chunks.pop_front();
                    _num_full_chunks.fetch_sub(1, std::memory_order_acq_rel);
                }
            }
            if (mem == nullptr) break; // no full chunks left

            // How much to read from this chunk?
            read += _working_chunk.extractFromChunkAndInsertRemaining(mem, numBytesInMem, out, totalLiteralLimit);

            // Transfer used chunk to chunk sink 
            _chunk_sink((uint8_t*)mem);
        }

        // Also flush the current working chunk as far as possible
        while (totalLiteralLimit == -1 || totalLiteralLimit > 0) {
            auto sizeBefore = out.size();
            bool success = _working_chunk.fetch(out, totalLiteralLimit);
            if (!success) break; // no clauses left
            read++;
            if (totalLiteralLimit != -1) 
                totalLiteralLimit -= getTrueExportedClauseLength(out[sizeBefore]);
        }

        return read;
    }

    uint8_t* tryStealChunk() {

        if (_num_full_chunks.load(std::memory_order_acq_rel) == 0)
            return nullptr;

        auto lock = _full_chunks_mutex.getLock();
        if (_full_chunks.empty()) return nullptr;

        auto [size, memory] = _full_chunks.front();
        _full_chunks.pop_front();
        _num_full_chunks.fetch_sub(1, std::memory_order_acq_rel);

        // At this point, clauses are deleted!
        _working_chunk.extractFromChunkAndConsume((int*)memory, size, _clause_deleter);
        
        return memory;
    }

    int getNumFullChunks() const {
        return _num_full_chunks.load(std::memory_order_relaxed);
    }

    void setClauseDeleter(std::function<void(Clause&)> clauseDeleter) {
        _clause_deleter = clauseDeleter;
    }

    BufferChunk::Mode getOperationMode() const {return _working_chunk.getOperationMode();}
    int getModeParam() const {return _working_chunk.getModeParam();}

    inline int getTrueExportedClauseLength(int firstInt) const {
        return _working_chunk.getTrueExportedClauseLength(firstInt);   
    }

    inline int getEffectiveExportedClauseLength(int firstInt) const {
        return _working_chunk.getEffectiveExportedClauseLength(firstInt);
    }

    inline int getMaxLbd() const {
        auto mode = getOperationMode();
        int modeParam = getModeParam();
        if (mode == BufferChunk::SAME_SUM) {
            return modeParam/2;
        }
        return modeParam;
    }
};
