
#ifndef DOMPASCH_MALLOB_WAITFREE_CLAUSE_DATABASE_HPP
#define DOMPASCH_MALLOB_WAITFREE_CLAUSE_DATABASE_HPP

#include <list>

#include "clause_slot.hpp"
#include "chunk_manager.hpp"
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
    struct InplaceClauseComparator {
        int clauseSizeIncludingLbd;
        InplaceClauseComparator(int clauseSizeIncludingLbd) 
            : clauseSizeIncludingLbd(clauseSizeIncludingLbd) {}
        bool operator()(const int* left, const int* right) const {
            if (left == right) return false;
            for (size_t i = 0; i < clauseSizeIncludingLbd; i++) {
                if (left[i] != right[i]) return left[i] < right[i];
            }
            return false;
        }
    };

    /*
    Initially all chunks are part of this manager (virtually and not yet allocated).
    Queries for new chunks go here, and unneeded chunks are returned here.
    */
    ChunkManager _chunk_mgr;
    /*
    There is one slot for each length-LBD combination up to a certain clause length
    (_max_lbd_partitioned_size) and then one slot for each clause length.
    A slot itself consists of a flexible number of memory chunks with one lock-free
    MPSC ring buffer operating on each chunk.
    */
    std::vector<ClauseSlot> _slots;

    int _max_lbd_partitioned_size;
    int _chunk_size;
    bool _use_checksum;

    ClauseHistogram _hist_deleted_in_slots;

public: 
    AdaptiveClauseDatabase(int maxClauseSize, int maxLbdPartitionedSize, int baseBufferSize, 
        int numChunks, int numProducers, bool useChecksum = false);
    ~AdaptiveClauseDatabase() = default;

    /*
    Insert a clause from a certain producer (0 <= ID < #producers).
    SUCCESS: The clause has been inserted.
    TRY_LATER: A spurious failure occurred due to the internal lock-free
    data structures: The clause should be deferred and inserted at a
    later time.
    DROP: The clause has been rejected due to lack of space relative 
    to the clause's importance and can be discarded (although it may 
    be possible to insert it later).
    In all cases, c can be freed or reused after calling this method.
    */
    AddClauseResult addClause(int producerId, const Clause& c);

    /*
    Add multiple clauses at once. More efficient than addClause for a series of clauses
    because the search for a fitting slot and chunk for each clause is not done redundantly.
    The clauses *must* be sorted with respect to the buckets of this instance, i.e.,
    best clauses come first, as retrieved by calling exportBuffer and then iterating
    over the clauses via getBufferReader.
    As in adding a single clause, spurious failures may occur causing a clause to be
    deferred - each such clause will be returned to the caller via the supplied clause list.
    A conditional can be supplied to this function which decides for each clause whether
    it should be inserted or not. It is ensured that this conditional is only called once
    for each clause.
    */
    void bulkAddClauses(int producerId, const std::vector<Clause>& clauses, std::list<Clause>& deferredOut, 
            SolvingStatistics& stats, std::function<bool(const Clause& c)> conditional = [](const Clause&) {return true;});

    void printChunks(int nextExportSize = -1);
    
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
            const int minClauseLength = -1, const int maxClauseLength = -1, 
            bool sortClauses = true);

    /*
    Allows to iterate over the clauses contained in a flat vector of integers
    as exported by exportBuffer. Must have been created by an AdaptiveClauseDatabase
    or a BufferMerger with the same parametrization as this instance.
    Throughout the life time of the BufferReader, the underlying vector must be valid.
    */
    BufferReader getBufferReader(int* begin, size_t size, bool useChecksums = false);
    BufferMerger getBufferMerger();

    ClauseHistogram& getDeletedClausesHistogram();

private:
    size_t getSlotIdx(int clauseSize, int lbd);

};

#endif
