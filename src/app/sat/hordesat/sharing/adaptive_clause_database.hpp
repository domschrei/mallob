
#ifndef DOMPASCH_MALLOB_WAITFREE_CLAUSE_DATABASE_HPP
#define DOMPASCH_MALLOB_WAITFREE_CLAUSE_DATABASE_HPP

#include <list>

//#include "clause_slot.hpp"
//#include "chunk_manager.hpp"
#include "database/buffer_slot.hpp"
#include "bucket_label.hpp"
#include "buffer_reader.hpp"
#include "buffer_merger.hpp"
#include "util/periodic_event.hpp"
#include "app/sat/hordesat/solvers/solver_statistics.hpp"

/*
Buffering structure for clauses which sorts and prioritizes clauses 
by length (primary) and LBD score (secondary). The structure is adaptive
because memory chunks of fixed size are allocated on demand and
can be moved freely from one length-LBD slot to another as necessary.
*/
class AdaptiveClauseDatabase {

public:
    enum AddClauseResult {SUCCESS, TRY_LATER, DROP};

private:

    // Compares two clauses alphanumerically.
    // The clauses are given as pointers to the raw literals, possibly
    // both with an LBD score at the front.
    // The effective size of the clauses must be equal to the member
    // supplied in the construction of the comparator.
    struct InplaceClauseComparatorUniformSize {
        int clauseSizeIncludingLbd;
        InplaceClauseComparatorUniformSize(int clauseSizeIncludingLbd) 
            : clauseSizeIncludingLbd(clauseSizeIncludingLbd) {}
        bool operator()(const int* left, const int* right) const {
            if (left == right) return false;
            for (size_t i = 0; i < clauseSizeIncludingLbd; i++) {
                if (left[i] != right[i]) return left[i] < right[i];
            }
            return false;
        }
    };

    struct InplaceClauseComparatorUniformSizeLbdSum {
        int sumOfLengthAndLbd;
        InplaceClauseComparatorUniformSizeLbdSum(int sumOfLengthAndLbd) 
            : sumOfLengthAndLbd(sumOfLengthAndLbd) {}
        bool operator()(const int* left, const int* right) const {
            if (left == right) return false;
            int sizeLeft = sumOfLengthAndLbd - left[0] + 1;
            int sizeRight = sumOfLengthAndLbd - right[0] + 1;
            if (sizeLeft != sizeRight) return sizeLeft < sizeRight;
            for (size_t i = 0; i < sizeLeft; i++) {
                if (left[i] != right[i]) return left[i] < right[i];
            }
            return false;
        }
    };

    std::atomic_int _num_free_chunks{0};
    Mutex _free_chunks_mutex;
    std::list<uint8_t*> _free_chunks;

    /*
    There is one slot for each length-LBD combination up to a certain clause length
    (_max_lbd_partitioned_size) and then one slot for each clause length.
    A slot itself consists of exactly one lock-free MPSC ring buffer and a flexible
    number of filled memory chunks.
    */
    std::vector<BufferSlot> _slots;
    std::vector<int> _slot_lbd;
    robin_hood::unordered_flat_map<std::pair<int, int>, int, IntPairHasher> _size_lbd_to_slot_idx;

    int _max_lbd_partitioned_size;
    int _max_clause_length;
    bool _slots_for_sum_of_length_and_lbd;

    int _chunk_size;
    bool _use_checksum;
    BucketLabel _bucket_iterator;

    ClauseHistogram _hist_deleted_in_slots;

public:
    struct Setup {
        int numProducers = 1;
        int numChunks = 0;
        int chunkSize = 1500;
        int maxClauseLength = 20;
        bool useChecksums = false;

        bool slotsForSumOfLengthAndLbd = false;
        int maxLbdPartitionedSize = 2;
    };

    AdaptiveClauseDatabase(Setup setup);
    ~AdaptiveClauseDatabase() = default;

    /*
    Insert a clause from a certain producer (0 <= ID < #producers).
    true: The clause has been inserted.
    false: The clause has been rejected due to lack of space relative 
    to the clause's importance and can be discarded (although it may 
    be possible to insert it later).
    In both cases, c can be freed or reused after calling this method.
    */
    bool addClause(int producerId, const Clause& c);

    /*
    Add multiple clauses at once. More efficient than addClause for a series of clauses
    because the search for a fitting slot and chunk for each clause is not done redundantly.
    The clauses *must* be sorted with respect to the buckets of this instance, i.e.,
    best clauses come first, as retrieved by calling exportBuffer and then iterating
    over the clauses via getBufferReader.
    A conditional can be supplied to this function which decides for each clause whether
    it should be inserted or not. It is ensured that this conditional is only called once
    for each clause.
    Returns: Number of successfully added clauses.
    */
    int bulkAddClauses(int producerId, const std::vector<Clause>& clauses,
            std::function<bool(const Clause& c)> conditional = [](const Clause&) {return true;});

    void printChunks(int nextExportSize = -1);
    
    enum ExportMode {UNITS, NONUNITS, ANY};
    /*
    Flushes the clauses of highest priority and writes them into a flat buffer.
    The buffer first contains a hash value of type size_t and then integers only.
    For each bucket (size,lbd), the buffer contains a number k followed by
    size*k integers representing each clause. For each bucket (size), the buffer
    contains a number k followed by (size+1)*k integers representing each clause,
    beginning with the LBD value. If minClauseLength and/or maxClauseLength is
    a positive integer, the buffer will only contain clauses adhering to the 
    respective limit.
    The buffer can be parsed via getBufferReader or merged with other buffers via
    getBufferMerger.
    */
    std::vector<int> exportBuffer(int sizeLimit, int& numExportedClauses, 
            ExportMode mode = ANY, bool sortClauses = true);

    /*
    Allows to iterate over the clauses contained in a flat vector of integers
    as exported by exportBuffer. Must have been created by an AdaptiveClauseDatabase
    or a BufferMerger with the same parametrization as this instance.
    Throughout the life time of the BufferReader, the underlying vector must be valid.
    */
    BufferReader getBufferReader(int* begin, size_t size, bool useChecksums = false);
    BufferMerger getBufferMerger(int sizeLimit);

    ClauseHistogram& getDeletedClausesHistogram();

private:
    size_t getSlotIdx(int clauseSize, int lbd);
    BucketLabel getBucketIterator();

};

#endif
