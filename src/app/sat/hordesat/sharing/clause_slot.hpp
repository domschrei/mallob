
#ifndef DOMPASCH_MALLOB_CLAUSE_SLOT_HPP
#define DOMPASCH_MALLOB_CLAUSE_SLOT_HPP

#include <list>
#include <functional>

#include "util/ringbuffer.hpp"
#include "util/sys/threading.hpp"
#include "util/logger.hpp"
#include "util/sys/atomics.hpp"
#include "app/sat/hordesat/solvers/solver_statistics.hpp"
#include "app/sat/hordesat/sharing/clause_histogram.hpp"

// Possible states of a chunk of a clause slot.
// A state s > 0 indicates that s threads are accessing the chunk.
#define SLOT_CHUNK_SENTINEL -4
#define SLOT_CHUNK_INVALID -3
#define SLOT_CHUNK_IN_CREATION -2
#define SLOT_CHUNK_IN_DELETION -1
#define SLOT_CHUNK_VALID 0

class ClauseSlot {

public:
    enum SlotResult {SUCCESS, SPURIOUS_FAIL, TOTAL_FAIL};

private:
    int _clause_size;
    int _lbd_value;
    bool _uniform_lbd_values;
    int _chunk_size;
    int _num_producers;

    std::atomic_int _num_chunks {0};

    struct Chunk {
        UniformSizeClauseRingBuffer* data = nullptr;
        std::atomic_int state {SLOT_CHUNK_INVALID};
        int unusedCounter = 0;
        std::atomic_int numElems {0};

        Chunk() = default;
        Chunk(UniformSizeClauseRingBuffer* data);
        Chunk(Chunk&& other);

        void swap(Chunk& other);
    };
    std::list<Chunk> _chunks;

    std::atomic_bool _acquiring_chunk {false};
    std::atomic_bool _modifying_list_end {false};
    std::atomic<std::list<Chunk>::iterator> _list_head;
    std::atomic<std::list<Chunk>::iterator> _list_tail;

    std::function<std::pair<SlotResult, int*>(void)> _chunk_source;
    std::function<void(int*)> _chunk_sink;

    int _clause_capacity_per_chunk;

    ClauseHistogram* _hist_deleted = nullptr;

public:
    ClauseSlot();
    ClauseSlot(int clauseSize, int lbdValue, bool uniformLbdValues, int chunkSize, int numProducers);
    ClauseSlot(ClauseSlot&& other);
    ~ClauseSlot();
    void init(bool emptyList);

    ClauseSlot& operator=(ClauseSlot&& other);
    void setChunkSource(std::function<std::pair<SlotResult, int*>(void)> chunkSource);
    void setChunkSink(std::function<void(int*)> chunkSink);
    SlotResult insert(int producerId, const Clause& clause);
    void insert(int producerId, const Clause*& begin, const Clause* end, std::list<Clause>& deferred,
            SolvingStatistics& stats, std::function<bool(const Clause& c)> conditional = [](const Clause&) {return true;});

    SlotResult insert(int producerId, std::function<bool(Chunk&)> inserter, bool isSecondTry = false);
    int getClauses(std::vector<int>& selection, int maxNumClauses);
    bool addChunk(int* data = nullptr);
    SlotResult releaseChunk(int*& data);
    int getNumActiveChunks();
    std::vector<int> getChunkFillStates();
    void setDeletedClausesHistogram(ClauseHistogram& hist);

private:

    std::list<Chunk>::iterator fwdIterator(bool unsafe = false);
    std::list<Chunk>::iterator backIterator(bool unsafe = false);
    bool isEnd(std::list<Chunk>::iterator it);

    bool appendAndUpdateIterators(int* data);
    bool isFull(Chunk& chunk);
    bool isEmpty(Chunk& chunk);
    UniformSizeClauseRingBuffer* allocateBuffer(int* data);
    bool switchChunkState(Chunk& chunk, int stateFrom, int stateTo, bool safeWithoutCompare = false, int* actualValue = nullptr);
    bool addToChunkState(Chunk& chunk, int offset, int* actualValue = nullptr);

    bool obtainListEndEditing();
    void returnListEndEditing();

    bool isClauseCompatible(const Clause& c);

};

#endif
