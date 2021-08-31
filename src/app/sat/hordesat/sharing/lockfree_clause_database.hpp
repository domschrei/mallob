
#ifndef DOMPASCH_MALLOB_LOCKFREE_CLAUSE_DATABASE_HPP
#define DOMPASCH_MALLOB_LOCKFREE_CLAUSE_DATABASE_HPP

#include <vector>
#include <algorithm>

#include "util/ringbuffer.hpp"
#include "util/hashing.hpp"
#include "app/sat/hordesat/utilities/clause_filter.hpp"
#include "util/logger.hpp"

#include "buffer_reader.hpp"
#include "buffer_merger.hpp"

class LockfreeClauseDatabase {

private:
    int _max_clause_size;
    int _max_lbd_partitioned_size;
    bool _use_checksum;

    std::vector<UniformSizeClauseRingBuffer*> _buffers;

    UniformSizeClauseRingBuffer NULL_BUFFER;

public:
    LockfreeClauseDatabase(int maxClauseSize, int maxLbdPartitionedSize, int baseBufferSize, int numProducers, bool useChecksum = false);
    ~LockfreeClauseDatabase();

    bool addClause(int producerId, const Clause& c);
    std::vector<int> exportBuffer(size_t sizeLimit, int& numExportedClauses);
    BufferReader getBufferReader(int* begin, size_t size, bool useChecksums = false);
    BufferMerger getBufferMerger();

private:
    UniformSizeClauseRingBuffer& getBuffer(int clauseSize, int lbd);
};

#endif
