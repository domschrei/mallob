
#include "buffer_reader.hpp"
#include "util/logger.hpp"

BufferReader::BufferReader(int* buffer, int size, int maxLbdPartitionedSize, bool useChecksum) : 
        _buffer(buffer), _size(size), 
        _max_lbd_partitioned_size(maxLbdPartitionedSize), _use_checksum(useChecksum) {
    
    int numInts = sizeof(size_t)/sizeof(int);
    if (_use_checksum && _size > 0) {
        // Extract checksum
        assert(size >= numInts);
        memcpy(&_true_hash, _buffer, sizeof(size_t));
    }

    _remaining_cls_of_bucket = _size <= numInts ? 0 : _buffer[numInts];
    assert(_remaining_cls_of_bucket >= 0);
    _current_pos = numInts+1;
    _hash = 1;
    _effective_clause_size = _bucket.size;
    _current_clause.size = _bucket.size;
    _current_clause.lbd = _bucket.lbd;
}

const Mallob::Clause& BufferReader::endReading() {
    // Verify checksum
    if (_use_checksum && _hash != _true_hash) {
        LOG(V0_CRIT, "[ERROR] Checksum fail\n");
        abort();
    }
    _buffer = nullptr;
    _current_clause.begin = nullptr;
    return _current_clause;
}
