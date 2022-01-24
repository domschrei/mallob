
#ifndef DOMPASCH_MALLOB_BUFFER_READER_HPP
#define DOMPASCH_MALLOB_BUFFER_READER_HPP

#include <cstring>

#include "util/assert.hpp"
#include "bucket_label.hpp"
#include "app/sat/hordesat/utilities/clause.hpp"
#include "util/hashing.hpp"
#include "util/logger.hpp"

class BufferReader {
private:
    int* _buffer = nullptr;
    size_t _size;
    int _max_lbd_partitioned_size;

    size_t _current_pos = 0;
    BucketLabel _bucket;
    size_t _remaining_cls_of_bucket = 0;
    int _effective_clause_size;
    Mallob::Clause _current_clause;

    bool _use_checksum;
    size_t _hash;
    size_t _true_hash = 1;

public:
    BufferReader() = default;
    BufferReader(int* buffer, int size, int maxLbdPartitionedSize, bool useChecksum = false);
    void releaseBuffer() {_buffer = nullptr;}
    Mallob::Clause* getCurrentClausePointer() {return &_current_clause;}
    size_t getRemainingSize() const {return _size - _current_pos;}
    
    inline const Mallob::Clause& getNextIncomingClause() {
        // No buffer?
        if (_buffer == nullptr) return _current_clause;

        // Find first bucket with some clauses left
        if (_remaining_cls_of_bucket == 0) {
            while (_remaining_cls_of_bucket == 0) {
                
                // Nothing left to read?
                if (_current_pos >= _size) {
                    return endReading();
                }

                // Go to next bucket
                _bucket.next(_max_lbd_partitioned_size);
                _remaining_cls_of_bucket = _buffer[_current_pos++];
                assert(_remaining_cls_of_bucket >= 0);
            }
            // Update clause data
            _effective_clause_size = _bucket.size + (_bucket.size <= _max_lbd_partitioned_size ? 0 : 1);
            _current_clause.size = _bucket.size;
            _current_clause.lbd = _bucket.lbd;
        }

        // Does clause exceed bounds of the buffer?
        if (_current_pos+_effective_clause_size > _size) {
            return endReading();
        }

        assert(_buffer[_current_pos] != 0 || 
            LOG_RETURN_FALSE("ERROR: Buffer is zero @ pos %i/%i (bucket (%i,%i))!\n", 
                _current_pos, _size, _bucket.size, _bucket.lbd));

        if (_use_checksum) {
            hash_combine(_hash, Mallob::ClauseHasher::hash(
                _buffer+_current_pos, _effective_clause_size, 3
            ));
        }

        if (_effective_clause_size > _bucket.size) {
            // Set explicit LBD value
            _current_clause.lbd = *(_buffer+_current_pos);
            _current_pos++;
        }
        // Set pointer to literals
        _current_clause.begin = _buffer+_current_pos;
        _current_pos += _bucket.size;

        // Decrement remaining clauses
        _remaining_cls_of_bucket--;

        return _current_clause;
    }

private:
    const Mallob::Clause& endReading();
};

#endif
