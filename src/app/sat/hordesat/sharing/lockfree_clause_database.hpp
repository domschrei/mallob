
#ifndef DOMPASCH_MALLOB_LOCKFREE_CLAUSE_DATABASE_HPP
#define DOMPASCH_MALLOB_LOCKFREE_CLAUSE_DATABASE_HPP

#include <vector>
#include <algorithm>

#include "util/ringbuffer.hpp"
#include "util/robin_hood.hpp"
#include "util/hashing.hpp"
#include "app/sat/hordesat/utilities/clause_filter.hpp"
#include "util/logger.hpp"

class LockfreeClauseDatabase {

public:
    struct BucketLabel {
        int size = 1;
        int lbd = 1;

        void next(int maxLbdPartitionedSize) {
            if (lbd == size || size > maxLbdPartitionedSize) {
                size++;
                lbd = 2;
            } else {
                lbd++;
            }
        }

        bool operator==(const BucketLabel& other) {
            return size == other.size && lbd == other.lbd;
        }
        bool operator!=(const BucketLabel& other) {
            return !(*this == other);
        }
    };
    
    class BufferReader {
    private:
        int* _buffer;
        size_t _size;
        int _max_lbd_partitioned_size;

        size_t _current_pos = 0;
        BucketLabel _bucket;
        size_t _remaining_cls_of_bucket = 0;

        size_t _hash;
        size_t _true_hash = 1;

    public:
        BufferReader(int* buffer, int size, int maxLbdPartitionedSize) : _buffer(buffer), _size(size), 
                _max_lbd_partitioned_size(maxLbdPartitionedSize) {
            
            int numInts = sizeof(size_t)/sizeof(int);
            if (_size > 0) {
                // Extract checksum
                assert(size >= numInts);
                memcpy(&_true_hash, _buffer, sizeof(size_t));
            }

            _remaining_cls_of_bucket = _size <= numInts ? 0 : _buffer[numInts];
            _current_pos = numInts+1;
            _hash = 1;
        }

        Clause getNextIncomingClause() {
            Clause cls {nullptr, 0, 0};

            // Find appropriate clause size
            while (_remaining_cls_of_bucket == 0) {
                
                // Nothing left to read?
                if (_current_pos >= _size) {
                    // Verify checksum
                    if (_hash != _true_hash) {
                        log(V0_CRIT, "Checksum fail!\n");
                        abort();
                    }
                    return cls;
                }

                // Go to next bucket
                _bucket.next(_max_lbd_partitioned_size);
                _remaining_cls_of_bucket = _buffer[_current_pos++];
            }

            if (_current_pos >= _size) return cls;
            bool partitionedByLbd = _bucket.size <= _max_lbd_partitioned_size;
            assert(_buffer[_current_pos] != 0);

            // Get start and stop index of next clause
            int start = _current_pos;
            int stop = start + _bucket.size;
            if (stop+(partitionedByLbd ? 0 : 1) > _size) return cls;

            hash_combine(_hash, ClauseHasher::hash(
                _buffer+start,
                _bucket.size+(partitionedByLbd ? 0 : 1), 3
            ));

            if (partitionedByLbd) {
                // set LBD value inferred from buffer structure
                cls.lbd = _bucket.lbd;
            } else {
                // read explicit LBD value
                cls.lbd = _buffer[start];
                start++;
                stop++;
            }
            _current_pos = stop;
            _remaining_cls_of_bucket--;

            // Return clause
            cls.begin = _buffer+start;
            cls.size = _bucket.size;
            return cls;
        }
    };

    class BufferMerger {
    private:
        int _max_lbd_partitioned_size;
        std::vector<BufferReader> _readers;
    public:
        BufferMerger(int maxLbdPartitionedSize) : _max_lbd_partitioned_size(maxLbdPartitionedSize) {}
        void add(BufferReader&& reader) {_readers.emplace_back(std::move(reader));}
        std::vector<int> merge(int sizeLimit) {
            
            ExactSortedClauseFilter _clause_filter;
            Clause nextClauses[_readers.size()];
            size_t hash = 1;

            std::vector<int> out;
            out.reserve(sizeLimit);
            for (int i = 0; i < sizeof(size_t)/sizeof(int); i++)
                out.push_back(0); // placeholder for checksum
            out.push_back(0); // counter for clauses of first bucket

            BucketLabel bucket;
            int counterPos = out.size()-1;

            while (true) {
                BucketLabel newBucket = bucket;

                // Fetch next clauses, get minimum size / lbd
                bool anyLeft = false;
                for (size_t i = 0; i < _readers.size(); i++) {
                    Clause& c = nextClauses[i];
                    if (c.size == -1) continue; // no clauses left

                    // If no clause is present, try to read the next one
                    if (c.begin == nullptr) {
                        c = _readers[i].getNextIncomingClause();
                        if (c.begin == nullptr) {
                            // no clauses left, set a magic number
                            c.size = -1;
                            continue;
                        }
                    }
                    
                    // Clause present - update bound on size and LBD
                    if (!anyLeft || c.size < newBucket.size || 
                            (c.size == newBucket.size 
                            && newBucket.size <= _max_lbd_partitioned_size 
                            && c.lbd < newBucket.lbd)) {
                        newBucket.size = c.size;
                        newBucket.lbd = c.lbd;
                    }
                    anyLeft = true;
                }
                
                // Check if any clauses are left
                if (!anyLeft) break;

                bool isLbdPartitioned = newBucket.size <= _max_lbd_partitioned_size;
                
                // Check if size of buffer would be exceeded with next clause
                if (out.size() + newBucket.size + (isLbdPartitioned?0:1) > sizeLimit)
                    break;

                assert(bucket.size <= newBucket.size
                        || log_return_false("lbd-partitioned: %s, (%i,%i) => (%i,%i)\n", 
                            isLbdPartitioned?"true":"false", bucket.size, bucket.lbd, 
                            newBucket.size, newBucket.lbd));
                if (bucket.size == newBucket.size) 
                    assert(!isLbdPartitioned || bucket.lbd <= newBucket.lbd
                        || log_return_false("lbd-partitioned: %s, (%i,%i) => (%i,%i)\n", 
                            isLbdPartitioned?"true":"false", bucket.size, bucket.lbd, 
                            newBucket.size, newBucket.lbd));

                // Skip empty intermediate buckets
                while (bucket.size != newBucket.size || (isLbdPartitioned && bucket.lbd != newBucket.lbd)) {
                    bucket.next(_max_lbd_partitioned_size);
                    out.push_back(0);
                    counterPos = out.size()-1;

                    // Reset clause filter
                    _clause_filter.clear();
                }
                bucket.lbd = newBucket.lbd; // if !isLbdPartitioned

                // Pick best clause(s) while checking for duplicates
                for (size_t i = 0; i < _readers.size(); i++) {
                    Clause& c = nextClauses[i];
                    if (c.begin == nullptr) continue;
                    if (c.size == bucket.size && c.lbd == bucket.lbd) {
                        if (_clause_filter.registerClause(c)) {
                            // Insert this clause
                            size_t sizeBefore = out.size();
                            if (!isLbdPartitioned) out.push_back(c.lbd);
                            out.insert(out.end(), c.begin, c.begin+c.size);
                            hash_combine(hash, ClauseHasher::hash(
                                out.data()+sizeBefore,
                                c.size+(isLbdPartitioned ? 0 : 1), 3
                            ));
                            out[counterPos]++;
                        }
                        // Reset clause slot
                        c.begin = nullptr;
                    }
                }
            }

            memcpy(out.data(), &hash, sizeof(size_t));
            return out;
        }
    };

private:
    int _max_clause_size;
    int _max_lbd_partitioned_size;

    std::vector<UniformSizeClauseRingBuffer*> _buffers;

    UniformSizeClauseRingBuffer NULL_BUFFER;

public:
    LockfreeClauseDatabase(int maxClauseSize, int maxLbdPartitionedSize, int baseBufferSize, int numProducers) 
                : _max_clause_size(maxClauseSize), _max_lbd_partitioned_size(std::max(2, maxLbdPartitionedSize)) {
        
        for (int clauseSize = 1; clauseSize <= _max_clause_size; clauseSize++) {
            if (clauseSize <= _max_lbd_partitioned_size) {
                // Create one bucket for each possible LBD value at this size
                for (int lbd = 2; lbd <= std::max(2, clauseSize); lbd++) {
                    _buffers.push_back(new UniformClauseRingBuffer(baseBufferSize, clauseSize, numProducers));
                }
            } else {
                // Create a single bucket for all clauses of this size
                _buffers.push_back(new UniformSizeClauseRingBuffer(baseBufferSize, clauseSize, numProducers));
            }
        }
    }

    bool addClause(int producerId, const Clause& c) {
        auto& buf = getBuffer(c.size, c.lbd);
        if (buf.isNull()) return false;
        //log(V4_VVER, "Storing clause of size %i, LBD %i ...\n", c.size, c.lbd);
        return buf.insertClause(c, producerId);
    }

    std::vector<int> exportBuffer(size_t sizeLimit, int& numExportedClauses) {
        int zero = 0;
        numExportedClauses = 0;
                    
        std::vector<int> selection;

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

            // Fetch correct buffer
            auto& buf = getBuffer(clauseSize, clauseLbd);
            if (buf.isNull()) break; // no more buffers left

            // Fetch all clauses in the buffer as long as there is space
            int effectiveClsLen = clauseSize + (partitionedByLbd ? 0 : 1);
            while (selection.size()+effectiveClsLen <= sizeLimit) {
                // Write next clause
                size_t sizeBefore = selection.size();
                if (!buf.getClause(selection)) break;
                std::sort(selection.begin()+sizeBefore+(partitionedByLbd ? 0 : 1), selection.end());
                selection[counterPos]++;
                numExportedClauses++;
                hash_combine(hash, ClauseHasher::hash(
                    selection.data()+sizeBefore,
                    clauseSize+(partitionedByLbd ? 0 : 1), 3
                ));
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

    BufferReader getBufferReader(int* begin, size_t size) {
        return BufferReader(begin, size, _max_lbd_partitioned_size);
    }

    BufferMerger getBufferMerger() {
        return BufferMerger(_max_lbd_partitioned_size);
    }

private:
    UniformSizeClauseRingBuffer& getBuffer(int clauseSize, int lbd) {
        size_t index = 0;
        // For all smaller clause sizes, add up the number of buckets
        for (int c = 1; c < clauseSize; c++) 
            index += (c <= _max_lbd_partitioned_size ? std::max(c-1,1) : 1);
        // If applicable, also add up the number of buckets of lower lbd
        if (clauseSize <= _max_lbd_partitioned_size) 
            for (int l = 2; l < lbd; l++) index++;
        if (index < _buffers.size()) return *_buffers[index];
        else return NULL_BUFFER;
    }
};

#endif
