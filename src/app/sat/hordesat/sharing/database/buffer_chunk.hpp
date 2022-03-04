
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

public:
    enum Mode {SAME_SUM, SAME_SIZE, SAME_SIZE_AND_LBD};

private:
    Mode _mode;
    int _mode_param;

    RingBufferV2* _ringbuf = nullptr;
    int _size;
    int _num_producers;

    std::atomic_int _edit_memory_state {MEM_STATE_AVAILABLE};

public:
    BufferChunk() {}
    BufferChunk(int size, Mode mode, int modeParam, int numProducers) :  
        _mode(mode),
        _mode_param(modeParam),
        _ringbuf(mode == SAME_SIZE_AND_LBD ? 
            new RingBufferV2(size, /*elemLength=*/modeParam, numProducers+1) : 
            new RingBufferV2(size, numProducers+1, nullptr)
        ), _size(size), _num_producers(numProducers) {}
    BufferChunk(BufferChunk&& moved) : 
        _mode(moved._mode), _mode_param(moved._mode_param), _ringbuf(moved._ringbuf), 
        _size(moved._size), _num_producers(moved._num_producers) {
        moved._size = 0;
        moved._ringbuf = nullptr;
    }
    ~BufferChunk() {
        if (_ringbuf != nullptr) delete _ringbuf;
    }
    
    bool tryInsert(const int* data, int producerId, int lbd = 0) {

        // Acquire reader status in memory state (busy waiting)
        acquireReaderStatus();

        bool success;
        if (useHeaderBytePerClause()) {
            
            assert((lbd > 0 && lbd <= 255) || log_return_false("[ERROR] lbd=%i @ insert - slot mode=%i param=%i\n", 
                    (int)lbd, _mode, _mode_param));

            uint8_t headerByte = (uint8_t) lbd;
            int length = getNumLiterals(lbd);
            success = _ringbuf->produce(data, producerId, length, headerByte);
        } else {
            success = _ringbuf->produce(data, producerId);
        }

        // Release reader status in memory state
        releaseReaderStatus();

        return success;
    }

    template <typename T>
    bool fetch(T& output, int maxNumLiterals = -1) {

        // Acquire reader status in memory state (busy waiting)
        acquireReaderStatus();

        bool success;
        if (useHeaderBytePerClause()) {
            uint8_t lbd;
            success = _ringbuf->getNextHeaderByte(lbd);
            if (success) {
                int elemLength = getNumLiterals(lbd);
                if (maxNumLiterals == -1 || elemLength <= maxNumLiterals) {
                    success = _ringbuf->consume(elemLength, output);
                    assert(success);
                } else success = false;
            }
        } else {
            if (maxNumLiterals == -1 || _mode_param <= maxNumLiterals) 
                success = _ringbuf->consume(output);
            else success = false;
        }

        // Release reader status in memory state
        releaseReaderStatus();

        return success;
    }

    size_t flushBuffer(uint8_t* out) {

        // Acquire writer status in memory state (busy waiting)
        acquireReaderStatus();

        uint8_t* currentPointer = out;
        uint8_t* end = out + sizeof(int)*_size;
        while (true) {
            auto remainingBytes = end - currentPointer;
            int remainingLits = (remainingBytes-1) / sizeof(int);
            if (remainingLits <= 0) break;
            bool success = fetch(currentPointer, remainingLits);
            if (!success) break;
        }
        assert(end - currentPointer >= 0);

        // Release writer status in memory state
        releaseReaderStatus();

        return currentPointer - out;
    }

    int extractFromChunkAndInsertRemaining(int* data, size_t numBytesInData, 
            std::vector<int>& out, int& totalLiteralLimit) {
        
        int read = 0;
        
        if (useHeaderBytePerClause()) {
            // Extract mixed clauses where a single header byte contains the LBD
            // and also encodes the clause's size 
            uint8_t* bytes = (uint8_t*) data;
            int byteCounter = 0;
            while (byteCounter < numBytesInData) {
                uint8_t lbd = bytes[byteCounter];
                
                if (lbd <= 0) {
                    std::string out;
                    for (size_t i = 0; i < numBytesInData; i++) out += std::to_string(bytes[i]) + " ";
                    assert(lbd > 0 || log_return_false("[ERROR] lbd=%i @ byte %i/%i of full chunk - slot mode=%i param=%i - %s\n", 
                        (int)lbd, byteCounter, numBytesInData, _mode, _mode_param, out.c_str()));
                }

                int elemLength = getNumLiterals(lbd);
                if (totalLiteralLimit == -1 || elemLength <= totalLiteralLimit) {
                    // Fits
                    out.push_back((int)lbd);
                    out.insert(out.end(), (int*)(bytes+byteCounter+1), ((int*)(bytes+byteCounter+1))+elemLength);
                    read++;
                    if (totalLiteralLimit != -1) totalLiteralLimit -= elemLength;
                } else {
                    // Does not fit any more: Try to insert into local chunk
                    tryInsert((int*) (bytes+byteCounter+1), _num_producers, lbd);
                }
                byteCounter += 1+elemLength*sizeof(int);
            }
        } else {
            // Extract clauses of fixed uniform size
            const int elemLength = _mode_param;
            int intCounter = 0;
            // While provided data buffer is not fully read
            while (sizeof(int)*(intCounter+elemLength) <= numBytesInData) {
                // Does next clause fit?
                if (totalLiteralLimit == -1 || elemLength <= totalLiteralLimit) {
                    // Fits
                    out.insert(out.end(), data+intCounter, data+intCounter+elemLength);
                    read++;
                    if (totalLiteralLimit != -1) totalLiteralLimit -= elemLength;
                } else {
                    // Does not fit any more: Try to insert into local chunk 
                    tryInsert(data+intCounter, _num_producers);
                }
                intCounter += elemLength;
            }
        }

        return read;
    }

    inline Mode getOperationMode() const {return _mode;}
    inline int getModeParam() const {return _mode_param;}

    inline int getTrueExportedClauseLength(int firstInt) const {
        if (_mode == BufferChunk::SAME_SUM) {
            return _mode_param - firstInt;
        }
        return _mode_param;
    }

    inline int getEffectiveExportedClauseLength(int firstInt) const {
        if (_mode == BufferChunk::SAME_SUM) {
            return _mode_param - firstInt + 1;
        }
        return _mode_param + (_mode == BufferChunk::SAME_SIZE_AND_LBD ? 0 : 1);
    }

private:

    inline bool useHeaderBytePerClause() const {
        return _mode != SAME_SIZE_AND_LBD;
    }

    inline int getNumLiterals(int lbd) const {
        if (_mode == BufferChunk::SAME_SUM) {
            int numLits = _mode_param - lbd;
            assert(lbd <= numLits || log_return_false("LBD=%i at SAME_SUM param=%i!\n", lbd, _mode_param));
            return numLits;
        }
        return _mode_param;
    }

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
