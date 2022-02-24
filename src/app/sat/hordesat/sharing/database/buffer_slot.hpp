
#pragma once

#include <list>
#include <functional>

#include "buffer_chunk.hpp"
#include "app/sat/hordesat/utilities/clause_filter.hpp"

class BufferSlot {

private:
    int _chunk_size;
    int _elem_length;
    int _num_producers;
    Mallob::Clause _template_clause;

    BufferChunk _working_chunk;
    std::atomic_int _num_full_chunks {0};
    Mutex _full_chunks_mutex;
    std::list<std::pair<size_t, uint8_t*>> _full_chunks;

    std::atomic_bool _replacing_chunk {false};
    std::function<uint8_t*()> _chunk_source;
    std::function<void(uint8_t*)> _chunk_sink;

public:
    BufferSlot() {}
    BufferSlot(int chunkSize, int elemLength, int clauseSize, int lbd, int numProducers) : 
        _chunk_size(chunkSize), _elem_length(elemLength), _num_producers(numProducers),
        _template_clause(nullptr, clauseSize, lbd), 
        _working_chunk(chunkSize, elemLength, (elemLength>clauseSize), numProducers) {}
    BufferSlot(BufferSlot&& moved) :
        _chunk_size(moved._chunk_size), _elem_length(moved._elem_length), _num_producers(moved._num_producers),
        _template_clause(moved._template_clause), _working_chunk(std::move(moved._working_chunk)), 
        _num_full_chunks(moved._num_full_chunks.load()), _full_chunks(moved._full_chunks),
        _chunk_source(moved._chunk_source), _chunk_sink(moved._chunk_sink) {}

    void setChunkSource(std::function<uint8_t*()> chunkSource) {_chunk_source = chunkSource;}
    void setChunkSink(std::function<void(uint8_t*)> chunkSink) {_chunk_sink = chunkSink;}

    bool insert(const Mallob::Clause& c, int producerId) {

        // Attempt to add the clause
        bool success = _working_chunk.tryInsert(c, producerId);
        while (!success) {

            // Not successful: chunk is (or was) full.
            // Try to replace it with a new chunk yourself:
            bool replacingChunk = false;
            if (_replacing_chunk.compare_exchange_strong(replacingChunk, true, std::memory_order_acquire)) {
                // Acquired exclusive right to replace chunk

                // Fetch a chunk
                uint8_t* flushedMemory = nullptr;
                size_t flushedSize;
                {
                    flushedMemory = _chunk_source();
                    if (flushedMemory != nullptr) {
                        // Flush the working chunk's buffer into the acquired chunk
                        flushedSize = _working_chunk.flushBuffer(flushedMemory);
                    }
                }

                // Release right to replace chunk
                replacingChunk = true;
                _replacing_chunk.compare_exchange_strong(replacingChunk, false, std::memory_order_release);
                
                LOG(V5_DEBG, "(%i,%i) FETCH CHUNK success=%i\n", 
                    _template_clause.size, _template_clause.lbd, flushedMemory == nullptr ? 0 : 1);
                
                // Did not get a chunk? Give up on inserting.
                if (flushedMemory == nullptr) return false;

                // Move the extracted full buffer to the list of full chunks
                auto lock = _full_chunks_mutex.getLock();
                _full_chunks.push_back(std::pair<size_t, uint8_t*>(flushedSize, flushedMemory));
                atomics::incrementRelaxed(_num_full_chunks);
            }

            // Retry to insert
            success = _working_chunk.tryInsert(c, producerId);
        }
        return true;
    }

    int flush(int desiredElems, std::vector<int>& out) {

        int read = 0;
        int numFullChunks = _num_full_chunks.load();

        // Loop over all available full chunks as long as remaining size permits
        while (desiredElems != 0 && _num_full_chunks.load(std::memory_order_relaxed) > 0) {
            
            // Try to fetch a full chunk
            int memSize = 0;
            int* mem = nullptr;
            {
                auto lock = _full_chunks_mutex.getLock();
                if (!_full_chunks.empty()) {
                    memSize = (int)_full_chunks.front().first;
                    mem = (int*)_full_chunks.front().second;
                    _full_chunks.pop_front();
                    atomics::decrementRelaxed(_num_full_chunks);
                }
            }
            if (mem == nullptr) break; // no full chunks left

            // How much to read from this chunk?
            int size = memSize;
            if (desiredElems > 0) size = std::min(desiredElems*_elem_length, size);

            // For each element in the memory of the chunk:
            size_t i = 0;
            while (i+_elem_length <= size) {
                // Write element into "out" vector
                for (int x = 0; x < _elem_length; x++) {
                    out.push_back(mem[i+x]);
                }
                i += _elem_length;
                read++;
                desiredElems--;
            }

            // Reinsert remaining elements into the current chunk
            while (i+_elem_length <= size) {
                // Write element into "out" vector
                _template_clause.begin = mem+i;
                // Write explicit LBD value if necessary
                if (_elem_length > _template_clause.size) {
                    _template_clause.lbd = _template_clause.begin[0];
                    _template_clause.begin++;
                }
                insert(_template_clause, _num_producers);
                i += _elem_length;
            }

            // Transfer used chunk to chunk sink 
            _chunk_sink((uint8_t*)mem);
        }

        // Also flush the current working chunk as far as possible
        while (desiredElems != 0) {
            bool success = _working_chunk.fetch(out);
            if (!success) break; // no clauses left
            read++;
            desiredElems--;
        }

        LOG(V5_DEBG, "(%i,%i) fullchunks=%i nread=%i\n", 
            _template_clause.size, _template_clause.lbd, numFullChunks, read);
        return read;
    }

    uint8_t* tryStealChunk() {

        if (_num_full_chunks.load(std::memory_order_relaxed) == 0)
            return nullptr;
        if (!_full_chunks_mutex.tryLock())
            return nullptr;
        if (_full_chunks.empty()) {
            _full_chunks_mutex.unlock();
            return nullptr;
        }

        auto [size, memory] = _full_chunks.front();
        _full_chunks.pop_front();
        atomics::decrementRelaxed(_num_full_chunks);
        _full_chunks_mutex.unlock();
        return memory;
    }

    int getNumFullChunks() const {
        return _num_full_chunks.load(std::memory_order_relaxed);
    }
};
