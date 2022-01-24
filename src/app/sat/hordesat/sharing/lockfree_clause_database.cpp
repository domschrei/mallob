
#include "lockfree_clause_database.hpp"

LockfreeClauseDatabase::LockfreeClauseDatabase(
    int maxClauseSize, int maxLbdPartitionedSize, int baseBufferSize, int numProducers, bool useChecksum) 
    : _base_buffer_size(baseBufferSize/3.0), _max_clause_size(maxClauseSize), 
        _max_lbd_partitioned_size(std::max(2, maxLbdPartitionedSize)), 
        _num_producers(numProducers), _use_checksum(useChecksum) {
    
    for (int clauseSize = 1; clauseSize <= _max_clause_size; clauseSize++) {
        if (clauseSize <= _max_lbd_partitioned_size) {
            // Create one bucket for each possible LBD value at this size
            for (int lbd = 2; lbd <= std::max(2, clauseSize); lbd++) {
                _buffers.emplace_back(1, new UniformClauseRingBuffer(_base_buffer_size, clauseSize, numProducers));
                _safe_buffer_sizes.push_back(new std::atomic_int(1));
                _allocating_new_buffer.push_back(new std::atomic_bool(false));
            }
        } else {
            // Create a single bucket for all clauses of this size
            _buffers.emplace_back(1, new UniformSizeClauseRingBuffer(_base_buffer_size, clauseSize, numProducers));
            _safe_buffer_sizes.push_back(new std::atomic_int(1));
            _allocating_new_buffer.push_back(new std::atomic_bool(false));
        }
    }
}

LockfreeClauseDatabase::~LockfreeClauseDatabase() {
    for (auto& buffers : _buffers) {
        for (auto& buf : buffers) {
            delete buf;
        }
    }
    for (auto& i : _safe_buffer_sizes) delete i;
    for (auto& b : _allocating_new_buffer) delete b;
}

LockfreeClauseDatabase::AddClauseResult LockfreeClauseDatabase::addClause(int producerId, const Clause& c) {
    size_t bufIdx = getBufferIdx(c.size, c.lbd);
    if (bufIdx < 0) return DROP;

    auto& bufs = _buffers[bufIdx];

    // Try to insert clause in one of the fitting buffers
    size_t i = 0;
    for (auto it = bufs.begin(); i < *_safe_buffer_sizes[bufIdx]; ++it) {
        UniformSizeClauseRingBuffer& buf = **it;
        if (buf.isNull()) continue;
        //log(V4_VVER, "Storing clause of size %i, LBD %i ...\n", c.size, c.lbd);
        if (buf.insertClause(c, producerId)) return SUCCESS;
        i++;
    }

    // No more space in the buffers for this clause

    if (i >= 7) {
        // This CDB slot is desperately overcrowded, stop wasting more memory on it
        return DROP;
    }
    
    // Check if this thread can allocate a new buffer right now
    bool expected = false;
    bool success = _allocating_new_buffer[bufIdx]->compare_exchange_strong(expected, true);
    if (!success) {
        // a larger buffer is already being allocated right now 
        assert(expected);
        return TRY_LATER; // TODO better handling?
    }

    // This thread now has the "lock" on creating a new buffer

    // Create buffer
    size_t factor = 1 << i;
    LOG(V4_VVER, "Create ringbuf of size %ld for clslen %i (lbd %i)\n", factor*_base_buffer_size, c.size, c.lbd);
    auto createdBuffer = createBuffer(c.size, factor);
    bufs.push_back(createdBuffer);
    (*_safe_buffer_sizes[bufIdx])++;
    
    // Release "lock"
    _allocating_new_buffer[bufIdx]->store(false);

    // Try inserting into new buffer
    return createdBuffer->insertClause(c, producerId) ? SUCCESS : TRY_LATER;
}

std::vector<int> LockfreeClauseDatabase::exportBuffer(size_t sizeLimit, int& numExportedClauses) {
    int zero = 0;
    numExportedClauses = 0;
                
    std::vector<int> selection;
    selection.reserve(sizeLimit);

    // Reserve space for checksum at the beginning of the buffer
    size_t hash = 1;
    int numInts = (sizeof(size_t)/sizeof(int));
    selection.resize(numInts);

    BucketLabel bucket;
    while (selection.size() < sizeLimit) {
        // Get size and LBD of current bucket
        int clauseSize = bucket.size;
        int clauseLbd = bucket.lbd;

        // Counter integer for the clauses of this bucket
        selection.push_back(0);
        int counterPos = selection.size()-1;

        bool partitionedByLbd = clauseSize <= _max_lbd_partitioned_size;

        // Fetch correct buffer list
        int bufIdx = getBufferIdx(clauseSize, clauseLbd);
        if (bufIdx < 0) break;
        int effectiveClsLen = clauseSize + (partitionedByLbd ? 0 : 1);
        auto& bufs = _buffers[bufIdx];

        // Empty each of the buffers until no space or no clauses left
        size_t i = 0;
        for (auto it = bufs.begin(); i < *_safe_buffer_sizes[bufIdx]; ++it) {
            auto& buf = **it;
            if (buf.isNull()) continue;

            // Fetch all clauses in the buffer as long as there is space
            while (selection.size()+effectiveClsLen <= sizeLimit) {
                // Write next clause
                size_t sizeBefore = selection.size();
                if (!buf.getClause(selection)) break; // no more clauses
                selection[counterPos]++;
                numExportedClauses++;
                if (_use_checksum) {
                    hash_combine(hash, ClauseHasher::hash(
                        selection.data()+sizeBefore,
                        clauseSize+(partitionedByLbd ? 0 : 1), 3
                    ));
                }
            }
            i++;
        }

        // Proceed to next bucket
        bucket.next(_max_lbd_partitioned_size);
    }

    // Remove trailing zeroes
    while (!selection.empty() && selection[selection.size()-1] == 0)
        selection.resize(selection.size()-1);

    // Write final hash checksum
    memcpy(selection.data(), &hash, sizeof(size_t));

    return selection;
}

BufferReader LockfreeClauseDatabase::getBufferReader(int* begin, size_t size, bool useChecksums) {
    return BufferReader(begin, size, _max_lbd_partitioned_size, useChecksums);
}

BufferMerger LockfreeClauseDatabase::getBufferMerger() {
    return BufferMerger(_max_lbd_partitioned_size, _use_checksum);
}

UniformSizeClauseRingBuffer* LockfreeClauseDatabase::createBuffer(int clauseSize, int bufSizeMultiplier) {
    if (clauseSize <= _max_lbd_partitioned_size) {
        return new UniformClauseRingBuffer(bufSizeMultiplier*_base_buffer_size, clauseSize, _num_producers);
    } else {
        return new UniformSizeClauseRingBuffer(bufSizeMultiplier*_base_buffer_size, clauseSize, _num_producers);
    }
}

size_t LockfreeClauseDatabase::getBufferIdx(int clauseSize, int lbd) {
    size_t index = 0;
    // For all smaller clause sizes, add up the number of buckets
    for (int c = 1; c < clauseSize; c++) 
        index += (c <= _max_lbd_partitioned_size ? std::max(c-1,1) : 1);
    // If applicable, also add up the number of buckets of lower lbd
    if (clauseSize <= _max_lbd_partitioned_size) 
        for (int l = 2; l < lbd; l++) index++;
    if (index < _buffers.size()) return index;
    else return -1;
}
