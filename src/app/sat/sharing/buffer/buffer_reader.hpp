
#pragma once

#include <cstring>

#include "util/assert.hpp"
#include "buffer_iterator.hpp"
#include "../../data/clause.hpp"
#include "util/hashing.hpp"
#include "util/logger.hpp"

class BufferReader {
private:
    int* _buffer = nullptr;
    size_t _size;

    size_t _current_pos = 0;
    BufferIterator _it;
    size_t _remaining_cls_of_bucket = 0;
    Mallob::Clause _current_clause;

    bool _use_checksum;
    size_t _hash;
    size_t _true_hash = 1;

public:
    BufferReader() = default;
    BufferReader(int* buffer, int size, int maxClauseLength, bool slotsForSumOfLengthAndLbd, bool useChecksum = false);

    void releaseBuffer() {_buffer = nullptr;}
    
    Mallob::Clause* getCurrentClausePointer() {return &_current_clause;}
    size_t getCurrentBufferPosition() const {return _current_pos;} 
    size_t getRemainingSize() const {return _size - _current_pos;}
    size_t getNumRemainingClausesInBucket() const {return _remaining_cls_of_bucket;}
    const BufferIterator& getCurrentBufferIterator() const {return _it;} 
    
    inline const Mallob::Clause& getNextIncomingClause() {
        // No buffer?
        if (_buffer == nullptr) return _current_clause;

        // Find first bucket with some clauses left
        if (_remaining_cls_of_bucket == 0) {
            do {    
                // Nothing left to read?
                if (_current_pos >= _size) {
                    return endReading();
                }

                // Go to next bucket
                _it.nextLengthLbdGroup();
                _remaining_cls_of_bucket = _buffer[_current_pos++];
                assert(_remaining_cls_of_bucket >= 0);
            
            } while (_remaining_cls_of_bucket == 0);

            // Update clause data
            _current_clause.size = _it.clauseLength;
            _current_clause.lbd = _it.lbd;
        }

        // Does clause exceed bounds of the buffer?
        if (_current_pos+_current_clause.size > _size) {
            return endReading();
        }

        if (_buffer[_current_pos] == 0) {
            std::string str; for (size_t i = 0; i < _size; i++) str += std::to_string(_buffer[i]) + " ";
            LOG(V0_CRIT, "ERROR: Buffer is zero @ pos %i/%i (bucket (%i,%i))! ===> %s\n", 
                _current_pos, _size, _it.clauseLength, _it.lbd, str.c_str());
            abort();
        }

        if (_use_checksum) {
            hash_combine(_hash, Mallob::ClauseHasher::hash(
                _buffer+_current_pos, _current_clause.size, 3
            ));
        }

        // Set pointer to literals
        _current_clause.begin = _buffer+_current_pos;
        _current_pos += _it.clauseLength;

        // Decrement remaining clauses
        _remaining_cls_of_bucket--;

        return _current_clause;
    }

private:
    const Mallob::Clause& endReading();
};
