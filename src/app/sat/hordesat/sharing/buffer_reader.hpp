
#ifndef DOMPASCH_MALLOB_BUFFER_READER_HPP
#define DOMPASCH_MALLOB_BUFFER_READER_HPP

#include <cstring>

#include "util/assert.hpp"
#include "bucket_label.hpp"
#include "app/sat/hordesat/utilities/clause.hpp"

class BufferReader {
private:
    int* _buffer;
    size_t _size;
    int _max_lbd_partitioned_size;

    size_t _current_pos = 0;
    BucketLabel _bucket;
    size_t _remaining_cls_of_bucket = 0;

    bool _use_checksum;
    size_t _hash;
    size_t _true_hash = 1;

public:
    BufferReader(int* buffer, int size, int maxLbdPartitionedSize, bool useChecksum = false);
    Mallob::Clause getNextIncomingClause();
};

#endif
