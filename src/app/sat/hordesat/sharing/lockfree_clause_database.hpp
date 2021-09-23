
#ifndef DOMPASCH_MALLOB_LOCKFREE_CLAUSE_DATABASE_HPP
#define DOMPASCH_MALLOB_LOCKFREE_CLAUSE_DATABASE_HPP

#include <vector>
#include <list>
#include <algorithm>

#include "util/ringbuffer.hpp"
#include "util/hashing.hpp"
#include "app/sat/hordesat/utilities/clause_filter.hpp"
#include "util/logger.hpp"

#include "buffer_reader.hpp"
#include "buffer_merger.hpp"

class LockfreeClauseDatabase {

private:
    int _base_buffer_size;
    int _max_clause_size;
    int _max_lbd_partitioned_size;
    int _num_producers;
    bool _use_checksum;

    std::vector<std::list<UniformSizeClauseRingBuffer*>> _buffers;
    std::vector<std::atomic_int*> _safe_buffer_sizes;
    std::vector<std::atomic_bool*> _allocating_new_buffer;

    UniformSizeClauseRingBuffer NULL_BUFFER;

public:
    LockfreeClauseDatabase(int maxClauseSize, int maxLbdPartitionedSize, int baseBufferSize, int numProducers, bool useChecksum = false);
    ~LockfreeClauseDatabase();

    enum AddClauseResult {SUCCESS, TRY_LATER, DROP};
    AddClauseResult addClause(int producerId, const Clause& c);
    
    std::vector<int> exportBuffer(size_t sizeLimit, int& numExportedClauses);
    BufferReader getBufferReader(int* begin, size_t size, bool useChecksums = false);
    BufferMerger getBufferMerger();

private:
    UniformSizeClauseRingBuffer* createBuffer(int clauseSize, int bufSizeMultiplier);
    size_t getBufferIdx(int clauseSize, int lbd);
};

#endif
