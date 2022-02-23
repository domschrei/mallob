
#pragma once

#include <malloc.h>
#include <cstring>
#include <vector>

#include "util/sys/atomics.hpp"
#include "util/sys/threading.hpp"
#include "util/ringbuffer.hpp"

#define MEM_STATE_EDITING -1
#define MEM_STATE_AVAILABLE 0

class BufferChunk {

private:
    UniformSizeClauseRingBuffer* _ringbuf = nullptr;
    int _size;
    int _elem_length;
    int _num_producers;

    std::atomic_int _edit_memory_state {MEM_STATE_AVAILABLE};

public:
    BufferChunk() {}
    BufferChunk(int size, int elemLength, bool explicitLbd, int numProducers) :  
        _ringbuf(explicitLbd ? 
            new UniformSizeClauseRingBuffer(size, elemLength-1, numProducers+1) : 
            new UniformClauseRingBuffer(size, elemLength, numProducers+1)
        ), _size(size), _elem_length(elemLength), _num_producers(numProducers) {

        LOG(V5_DEBG, "size=%i elemLen=%i explicitLbd=%i\n", size, elemLength, explicitLbd?1:0);
    }
    BufferChunk(BufferChunk&& moved) : 
        _ringbuf(moved._ringbuf), _size(moved._size), 
        _elem_length(moved._elem_length), _num_producers(moved._num_producers) {
        moved._size = 0;
        moved._ringbuf = nullptr;
    }
    ~BufferChunk() {
        if (_ringbuf != nullptr) delete _ringbuf;
    }
    
    bool tryInsert(const Mallob::Clause& c, int producerId) {

        // Acquire reader status in memory state (busy waiting)
        acquireReaderStatus();

        bool success = _ringbuf->insertClause(c, producerId);

        // Release reader status in memory state
        releaseReaderStatus();

        return success;
    }

    bool fetch(std::vector<int>& vec) {
        size_t sizeBefore = vec.size();
        bool success = _ringbuf->getClause(vec);
        if (success) assert(vec.size() == sizeBefore+_elem_length || log_return_false("%i != %i+%i\n", vec.size(), sizeBefore, _elem_length));
        return success;
    }

    size_t flushBuffer(uint8_t* swappedMemory) {

        // Acquire writer status in memory state (busy waiting)
        acquireWriterStatus();

        // Swap out the current memory with the provided "empty" swap memory
        size_t size = _ringbuf->flushBufferUnsafe(swappedMemory);

        // Release writer status in memory state
        releaseWriterStatus();

        return size;
    }

private:

    void acquireReaderStatus() {
        // Want to go from a number equal or larger than AVAILABLE to that number +1 
        int memstate = std::max(MEM_STATE_AVAILABLE, _edit_memory_state.load(std::memory_order_relaxed));
        while (!_edit_memory_state.compare_exchange_strong(memstate, memstate+1, std::memory_order_acquire)) {
            memstate = std::max(MEM_STATE_AVAILABLE, memstate);
        }
    }
    void releaseReaderStatus() {
        // Want to decrement the variable
        int memstate = _edit_memory_state.load(std::memory_order_relaxed);
        while (!_edit_memory_state.compare_exchange_strong(memstate, memstate-1, std::memory_order_release)) {}
    }

    void acquireWriterStatus() {
        // Want to set the variable from AVAILABLE to EDITING
        int memstate = MEM_STATE_AVAILABLE;
        while (!_edit_memory_state.compare_exchange_strong(memstate, MEM_STATE_EDITING, std::memory_order_acquire)) {
            memstate = MEM_STATE_AVAILABLE;
        }
    }
    void releaseWriterStatus() {
        // Want to set the variable from EDITING to AVAILABLE
        int memstate = MEM_STATE_EDITING;
        while (!_edit_memory_state.compare_exchange_strong(memstate, MEM_STATE_AVAILABLE, std::memory_order_release)) {}
    }
};
