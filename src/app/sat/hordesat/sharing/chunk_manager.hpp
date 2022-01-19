
#ifndef DOMPASCH_MALLOB_CHUNK_MANAGER_HPP
#define DOMPASCH_MALLOB_CHUNK_MANAGER_HPP

#include <vector>
#include <cstdlib>

#include "util/logger.hpp"
#include "util/assert.hpp"
#include "util/sys/atomics.hpp"

#define CHUNK_STATE_INVALID 0
#define CHUNK_STATE_VALID 1
#define CHUNK_STATE_ACCESSED 2

class ChunkManager {

private:
    std::vector<int*> _chunks;
    std::vector<std::atomic_int*> _chunk_states;

    std::atomic_int _num_allocatable;
    size_t _size_per_chunk;
    std::atomic_int _num_available {0};

public:
    ChunkManager() = default;
    ChunkManager(int numChunks, int sizePerChunk) : 
        _chunks(numChunks, nullptr), 
        _chunk_states(numChunks, nullptr), 
        _num_allocatable(numChunks), 
        _size_per_chunk(sizePerChunk) {

        for (size_t i = 0; i < _chunk_states.size(); i++)
            _chunk_states[i] = new std::atomic_int(CHUNK_STATE_INVALID);
    }
    
    ChunkManager& operator=(ChunkManager&& other) {
        _chunks = std::move(other._chunks);
        _chunk_states = std::move(other._chunk_states);
        _num_allocatable.store(other._num_allocatable.load(std::memory_order_relaxed), std::memory_order_relaxed);
        _size_per_chunk = other._size_per_chunk;
        _num_available.store(other._num_available.load(std::memory_order_relaxed), std::memory_order_relaxed);
        return *this;
    }

    ~ChunkManager() {
        for (size_t i = 0; i < _chunks.size(); i++) {
            if (_chunks[i]) free(_chunks[i]);
            delete _chunk_states[i];
        }
    }

    int* getChunkOrNullptr() {
        //log(V2_INFO, "CMGR getChunkOrNullptr\n");

        // Retrieve number of currently available chunks, try to allocate if needed
        if (_num_available.load(std::memory_order_relaxed) == 0) tryToAllocateChunk();
        int numAvailable = _num_available.load(std::memory_order_relaxed);
        if (numAvailable <= 0) return nullptr;
        
        // Try to exclusively access a chunk
        int idx = numAvailable-1;
        while (!switchChunkState(idx, CHUNK_STATE_VALID, CHUNK_STATE_ACCESSED)) {
            if (_num_available.load(std::memory_order_relaxed) == 0) return nullptr;
            idx--;
            if (idx < 0) idx = _chunks.size()-1;
        }
        
        // Can safely take chunk at position and set to invalid
        int* chunk = _chunks[idx];
        assert(chunk != nullptr);
        _chunks[idx] = nullptr;
        switchChunkState(idx, CHUNK_STATE_ACCESSED, CHUNK_STATE_INVALID);
        atomics::decrementRelaxed(_num_available);
        return chunk;
    }

    void insertChunk(int* data) {
        //log(V2_INFO, "CMGR insertChunk\n");
        assert(data != nullptr);
        doInsert(data);
    }

private:
    void tryToAllocateChunk() {
        //log(V2_INFO, "CMGR tryToAllocateChunk\n");

        // Find out if there is budget left & decrease it
        int numAllocatable = _num_allocatable.load(std::memory_order_relaxed);
        if (numAllocatable == 0) return;
        while (!_num_allocatable.compare_exchange_strong(numAllocatable, numAllocatable-1, std::memory_order_acq_rel)) {
            // Budget of chunks exhausted?
            if (numAllocatable == 0) return;
        }
        // -- successfully allocated a unit of budget

        // Allocate and insert new chunk
        doInsert((int*) malloc(sizeof(int) * _size_per_chunk));
        //log(V2_INFO, "CMGR +1 %i chunks alloc'd\n", _chunks.size()-(int)_num_allocatable);
    }

    void doInsert(int* chunkData) {
        
        // Where to insert the chunk?
        int idx = _num_available.load(std::memory_order_relaxed);
        while (!switchChunkState(idx, CHUNK_STATE_INVALID, CHUNK_STATE_ACCESSED)) {
            idx++;
            if (idx >= _chunks.size()) idx = 0;
        }
        assert(idx >= 0);
        assert(idx < _chunks.size());
        // -- successfully found and locked a valid insertion point

        // Safely insert new chunk at position
        _chunks[idx] = chunkData;
        switchChunkState(idx, CHUNK_STATE_ACCESSED, CHUNK_STATE_VALID);
        atomics::incrementRelaxed(_num_available);
    }

    bool switchChunkState(int idx, int fromState, int toState) {
        assert(idx >= 0);
        assert(idx < _chunk_states.size());
        int state = _chunk_states[idx]->load(std::memory_order_relaxed);
        if (state != fromState) return false;
        return (_chunk_states[idx]->compare_exchange_strong(state, toState, std::memory_order_acq_rel));
        //log(V2_INFO, "CSL(%i,%i) +A %p\n", _clause_size, _lbd_value, (void*)chunk.data);
    }
};

#endif
