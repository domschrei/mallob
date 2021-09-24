
#ifndef DOMPASCH_MALLOB_CLAUSE_SLOT_HPP
#define DOMPASCH_MALLOB_CLAUSE_SLOT_HPP

#include <list>
#include <shared_mutex>
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
        Chunk(UniformSizeClauseRingBuffer* data) : data(data) {
            state.store(SLOT_CHUNK_VALID, std::memory_order_relaxed);
        }
        Chunk(Chunk&& other) : data(other.data), state(other.state.load(std::memory_order_relaxed)),
                unusedCounter(other.unusedCounter), numElems(other.numElems.load(std::memory_order_relaxed)) {}

        void swap(Chunk& other) {
            auto dataTmp = data;
            auto stateTmp = state.load(std::memory_order_relaxed);
            auto unusedCounterTmp = unusedCounter;
            auto numElemsTmp = numElems.load(std::memory_order_relaxed);
            
            data = other.data;
            unusedCounter = other.unusedCounter;
            numElems.store(other.numElems.load(std::memory_order_relaxed), std::memory_order_relaxed);
            
            other.data = dataTmp;
            other.unusedCounter = unusedCounterTmp;
            other.numElems.store(numElemsTmp, std::memory_order_relaxed);
            
            state.store(other.state.load(std::memory_order_relaxed), std::memory_order_release);
            other.state.store(stateTmp, std::memory_order_release);
        }
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
    ClauseSlot() {
        init(/*emptyList=*/true);
    }
    ClauseSlot(int clauseSize, int lbdValue, bool uniformLbdValues, int chunkSize, int numProducers) : 
        _clause_size(clauseSize), _lbd_value(lbdValue), _uniform_lbd_values(uniformLbdValues), 
        _chunk_size(chunkSize), _num_producers(numProducers) {

        _clause_capacity_per_chunk = _chunk_size / (_clause_size + (_uniform_lbd_values ? 0 : 1));
        init(/*emptyList=*/true);
    }
    ClauseSlot(ClauseSlot&& other) {
        *this = std::move(other);
        init(/*emptyList=*/false);
    }
    ~ClauseSlot() {
        for (auto& chunk : _chunks) if (chunk.data) delete chunk.data;
    }

    void init(bool emptyList) {
        
        if (emptyList) {
            // Sentinels for begin and end
            _chunks.emplace_back();
            _chunks.back().state.store(SLOT_CHUNK_SENTINEL, std::memory_order_relaxed);
            _chunks.emplace_back();
            _chunks.back().state.store(SLOT_CHUNK_SENTINEL, std::memory_order_relaxed);
        }
        
        auto it = _chunks.begin();
        _list_head.store(it, std::memory_order_relaxed);
        it = _chunks.end(); --it;
        _list_tail.store(it, std::memory_order_relaxed);
    }

    ClauseSlot& operator=(ClauseSlot&& other) { 
        _clause_size = other._clause_size;
        _lbd_value = other._lbd_value;
        _uniform_lbd_values = other._uniform_lbd_values; 
        _chunk_size = other._chunk_size; 
        _num_producers = other._num_producers;
        _num_chunks = (int) other._num_chunks;
        _chunks = std::move(other._chunks);
        _chunk_sink = other._chunk_sink;
        _chunk_source = other._chunk_source;
        _clause_capacity_per_chunk = other._clause_capacity_per_chunk;
        _list_head.store(_chunks.begin(), std::memory_order_relaxed);
        auto it = _chunks.end();
        it--;
        _list_tail.store(it, std::memory_order_relaxed);
        return *this;
    }

    void setChunkSource(std::function<std::pair<SlotResult, int*>(void)> chunkSource) {
        _chunk_source = chunkSource;
    }

    void setChunkSink(std::function<void(int*)> chunkSink) {
        _chunk_sink = chunkSink;
    }

    SlotResult insert(int producerId, const Clause& clause) {
        return insert(producerId, [producerId, &clause](Chunk& chunk) {
            assert(chunk.data != nullptr);
            bool inserted = chunk.data->insertClause(clause, producerId);
            if (inserted) atomics::incrementRelaxed(chunk.numElems);
            return inserted;
        });
    }

    void insert(int producerId, const Clause*& begin, const Clause* end, std::vector<Clause>& deferred,
            SolvingStatistics& stats,
            std::function<bool(const Clause& c)> conditional = [](const Clause&) {return true;}) {
        
        const Clause* cPtr = begin;
        auto result = insert(producerId, [this, producerId, &cPtr, end, conditional, &stats](Chunk& chunk) {
            int numInserted = 0;
            bool finished = false;
            while (cPtr != end) {
                const Clause& c = *cPtr;
                assert(c.begin != nullptr);

                for (size_t i = 0; i < c.size; i++) assert(c.begin[i] != 0);
                if (!isClauseCompatible(c)) {
                    // read everything you possibly could in this slot
                    finished = true;
                    break;
                } 
                if (!conditional(c)) {
                    stats.receivedClausesFiltered++;
                    cPtr++;
                    continue; // skip this clause - condition not met
                }
                
                // cannot insert: buffer full -> give up.
                if (!chunk.data->insertClause(c, producerId)) break;
                
                numInserted++;
                cPtr++;
            }
            if (numInserted > 0) 
                chunk.numElems.fetch_add(numInserted, std::memory_order_relaxed);
            stats.receivedClausesInserted += numInserted;
            return finished;
        });

        // Defer or discard all clauses meeting the condition which have not been inserted
        while (cPtr != end) {
            if (!isClauseCompatible(*cPtr)) break;
            
            // Compatible clause did not make it: not entirely successful
            assert(result != SUCCESS);
            if (conditional(*cPtr)) {
                if (result == SPURIOUS_FAIL) {
                    // Spurious failure: defer clause
                    deferred.push_back(cPtr->copy());
                    cPtr->assertNonZeroLiterals();
                    stats.deferredClauses++;
                } else {
                    // Failure due to lack of space: discard clause
                    stats.discardedClauses++;
                }
            } else stats.receivedClausesFiltered++;
            cPtr++;
        }
        
        begin = cPtr;
    }

    SlotResult insert(int producerId, std::function<bool(Chunk&)> inserter, bool isSecondTry = false) {
        //log(V2_INFO, "CSL(%i,%i) insert(%i)\n", _clause_size, _lbd_value, producerId);
        
        Chunk* firstSeenInvalidChunk = nullptr;
        bool someChunkBusy = false;

        for (auto it = fwdIterator(); !isEnd(it); ++it) {
            Chunk& chunk = *it;

            if (isFull(chunk)) continue;

            // Cannot access as a valid chunk?
            int actualState;
            if (!addToChunkState(chunk, 1, &actualState)) {
                someChunkBusy = someChunkBusy || actualState != SLOT_CHUNK_INVALID;
                // Remember invalid chunk
                if (firstSeenInvalidChunk == nullptr && actualState == SLOT_CHUNK_INVALID)
                    firstSeenInvalidChunk = &chunk;
                continue;
            }

            // Try to insert clause, return "lock"
            bool finished = inserter(chunk);
            addToChunkState(chunk, -1);

            if (firstSeenInvalidChunk != nullptr) {
                // Swap current valid chunk with previously encountered invalid chunk

                if (switchChunkState(chunk, SLOT_CHUNK_VALID, SLOT_CHUNK_IN_CREATION)) {
                    if (switchChunkState(*firstSeenInvalidChunk, SLOT_CHUNK_INVALID, SLOT_CHUNK_IN_DELETION)) {
                        // Can swap
                        chunk.swap(*firstSeenInvalidChunk);
                        switchChunkState(chunk, SLOT_CHUNK_IN_DELETION, SLOT_CHUNK_INVALID, /*safeWithoutCompare=*/true);
                        switchChunkState(*firstSeenInvalidChunk, SLOT_CHUNK_IN_CREATION, SLOT_CHUNK_VALID, /*safeWithoutCompare=*/true);
                        firstSeenInvalidChunk = nullptr;
                    } else {
                        // Cannot swap: Reset chunk states
                        switchChunkState(chunk, SLOT_CHUNK_IN_CREATION, SLOT_CHUNK_VALID, /*safeWithoutCompare=*/true);
                        switchChunkState(*firstSeenInvalidChunk, SLOT_CHUNK_IN_DELETION, SLOT_CHUNK_INVALID, /*safeWithoutCompare=*/true);
                    }
                }
            }

            if (finished) return SUCCESS;
        }

        // Already tried to allocate a chunk: return here
        if (isSecondTry) return someChunkBusy ? SPURIOUS_FAIL : TOTAL_FAIL;
        
        // Some chunk was busy: Do not try to fetch more space but retry later
        if (someChunkBusy) return SPURIOUS_FAIL;

        // No chunk was busy: There is not enough space for this clause in this slot
        // Attempt to acquire a new chunk for this slot
        bool acquiringChunk = _acquiring_chunk.load(std::memory_order_relaxed);
        if (!acquiringChunk && _acquiring_chunk.compare_exchange_strong(acquiringChunk, true, std::memory_order_relaxed)) {
            // Obtained exclusive right to fetch a new chunk
            auto [result, data] = _chunk_source();
            if (result == TOTAL_FAIL) {
                _acquiring_chunk.store(false, std::memory_order_relaxed);
                return TOTAL_FAIL;
            } else if (result == SPURIOUS_FAIL) {
                _acquiring_chunk.store(false, std::memory_order_relaxed);
                return SPURIOUS_FAIL;
            } else {
                // Success!
                // Try to add chunk to slot
                bool success = addChunk(data);
                if (!success) {
                    // return chunk, fail "spuriously"
                    _chunk_sink(data);
                    _acquiring_chunk.store(false, std::memory_order_relaxed);
                    return SPURIOUS_FAIL;
                }
                // Adding chunk succeeded
                _acquiring_chunk.store(false, std::memory_order_relaxed);
                auto result = insert(producerId, inserter, /*isSecondTry=*/true);
                return result;
            }
        }

        // Return spurious failure if either some chunk was busy or a chunk allocation is going on
        return (someChunkBusy || acquiringChunk) ? SPURIOUS_FAIL : TOTAL_FAIL;
    }

    int getClauses(std::vector<int>& selection, int maxNumClauses) {
        //log(V2_INFO, "CSL(%i,%i) getClauses(%i)\n", _clause_size, _lbd_value, maxNumClauses);

        int received = 0;
        for (auto it = backIterator(); !isEnd(it); it--) {
            Chunk& chunk = *it;

            // Enter chunk
            if (addToChunkState(chunk, 1)) {

                // Extract as many clauses as fitting and possible
                assert(chunk.data != nullptr);
                
                if (isEmpty(chunk)) {
                    chunk.unusedCounter++;
                } else {
                    int receivedBefore = received;
                    while ((maxNumClauses < 0 || received < maxNumClauses) && chunk.data->getClause(selection)) {
                        for (size_t i = 0; i < _clause_size; i++) 
                            assert(selection[selection.size()-1-i] != 0);
                        atomics::decrementRelaxed(chunk.numElems);
                        received++;
                    }
                    if (receivedBefore < received) chunk.unusedCounter = 0; // reset unused counter
                }
                bool stale = chunk.unusedCounter >= 5;
                // Leave chunk
                addToChunkState(chunk, -1);
            
                // If the chunk became stale, try to remove it
                if (stale && switchChunkState(chunk, SLOT_CHUNK_VALID, SLOT_CHUNK_IN_DELETION)) {
                    atomics::decrementRelaxed(_num_chunks);
                    int* data = (int*) chunk.data->releaseBuffer();
                    _chunk_sink(data);
                    delete chunk.data;
                    chunk.data = nullptr;
                    chunk.unusedCounter = 0;
                    //log(V2_INFO, "CSL(%i,%i) -1: %i chunks\n", _clause_size, _lbd_value, (int)_num_chunks);
                    switchChunkState(chunk, SLOT_CHUNK_IN_DELETION, SLOT_CHUNK_INVALID, /*safeWithoutCompare=*/true);
                }
            }
        }

        return received;
    }

    bool addChunk(int* data = nullptr) {
        
        for (auto it = fwdIterator(); !isEnd(it); ++it) {
            auto& chunk = *it;
            if (!switchChunkState(chunk, SLOT_CHUNK_INVALID, SLOT_CHUNK_IN_CREATION))
                continue;
            
            // Found a place to insert new chunk
            chunk.data = allocateBuffer(data);
            assert(chunk.data != nullptr);
            chunk.numElems.store(0, std::memory_order_relaxed);
            atomics::incrementRelaxed(_num_chunks);
            switchChunkState(chunk, SLOT_CHUNK_IN_CREATION, SLOT_CHUNK_VALID, /*safeWithoutCompare=*/true);
            //log(V2_INFO, "CSL(%i,%i) +1: %i chunks\n", _clause_size, _lbd_value, (int)_num_chunks);
            return true;
        }

        // Could not find empty list item: try to create new one
        return appendAndUpdateIterators(data);
    }

    SlotResult releaseChunk(int*& data) {

        if (_num_chunks.load(std::memory_order_relaxed) <= 0) return SlotResult::TOTAL_FAIL;

        bool someChunkBusy = false;
        auto it = backIterator();
        for (; !isEnd(it); it--) {
            auto& chunk = *it;
            
            // Found a valid chunk to remove?
            int actualState;
            if (switchChunkState(chunk, SLOT_CHUNK_VALID, SLOT_CHUNK_IN_DELETION, false, &actualState)) 
                break;
            // Hit a valid chunk, but cannot remove? (in use right now)
            if (actualState > SLOT_CHUNK_VALID)
                someChunkBusy = true;
        }


        // Nothing found? Fail "spuriously" if some chunk was busy
        if (isEnd(it)) return someChunkBusy ? SPURIOUS_FAIL : TOTAL_FAIL;

        atomics::decrementRelaxed(_num_chunks);
        auto& chunk = *it;
        int numElems = chunk.numElems.load(std::memory_order_acquire);
        if (_hist_deleted) _hist_deleted->increase(_clause_size, numElems);
        data = (int*) chunk.data->releaseBuffer();
        delete chunk.data;
        chunk.data = nullptr;
        chunk.numElems.store(0, std::memory_order_relaxed);
        switchChunkState(chunk, SLOT_CHUNK_IN_DELETION, SLOT_CHUNK_INVALID, /*safeWithoutCompare=*/true);
        //log(V2_INFO, "CSL(%i,%i) -1: %i chunks\n", _clause_size, _lbd_value, (int)_num_chunks);
        return SUCCESS;
    }

    int getNumActiveChunks() {
        return std::max(0, _num_chunks.load(std::memory_order_relaxed));
    }

    std::vector<int> getChunkFillStates() {
        std::vector<int> out;
        for (auto it = fwdIterator(); !isEnd(it); ++it) {
            auto& chunk = *it;
            out.push_back(chunk.state == SLOT_CHUNK_INVALID ? -1 : chunk.numElems.load(std::memory_order_relaxed));
        }
        return out;
    }

    void setDeletedClausesHistogram(ClauseHistogram& hist) {
        _hist_deleted = &hist;
    }

private:

    std::list<Chunk>::iterator fwdIterator(bool unsafe = false) {
        //if (!unsafe) while (!obtainListEndEditing()) ;
        auto it = _list_head.load(std::memory_order_acquire);
        ++it;
        //if (!unsafe) returnListEndEditing();
        return it;
    }
    std::list<Chunk>::iterator backIterator(bool unsafe = false) {
        //if (!unsafe) while (!obtainListEndEditing()) ;
        auto it = _list_tail.load(std::memory_order_acquire);
        --it;
        //if (!unsafe) returnListEndEditing();
        return it;
    }
    bool isEnd(std::list<Chunk>::iterator it) {
        return it->state.load(std::memory_order_acquire) == SLOT_CHUNK_SENTINEL;
    }

    bool appendAndUpdateIterators(int* data) {

        if (!obtainListEndEditing()) return false;
        _chunks.emplace_back(allocateBuffer(data));
        returnListEndEditing();
        
        // Swap sentinel with inserted chunk
        (--_chunks.end())->swap(*--(--_chunks.end()));

        // Update tail pointer
        auto it = _chunks.end(); --it;
        _list_tail.store(it, std::memory_order_release);

        atomics::incrementRelaxed(_num_chunks);
        return true;
    }

    bool isFull(Chunk& chunk) {
        return chunk.numElems.load(std::memory_order_relaxed) >= _clause_capacity_per_chunk;
    }
    bool isEmpty(Chunk& chunk) {
        return chunk.numElems.load(std::memory_order_relaxed) <= 0;
    }

    UniformSizeClauseRingBuffer* allocateBuffer(int* data) {
        if (_uniform_lbd_values)
            return new UniformClauseRingBuffer(_chunk_size, _clause_size, _num_producers, (uint8_t*)data);
        else
            return new UniformSizeClauseRingBuffer(_chunk_size, _clause_size, _num_producers, (uint8_t*)data);
    }

    bool switchChunkState(Chunk& chunk, int stateFrom, int stateTo, bool safeWithoutCompare = false, int* actualValue = nullptr) {
        int state = chunk.state.load(std::memory_order_relaxed);
        if (safeWithoutCompare) {
            assert(state == stateFrom);
            chunk.state.store(stateTo, std::memory_order_acq_rel);
            return true;
        }
        if (state != stateFrom) {
            if (actualValue != nullptr) *actualValue = state;
            return false;
        }
        bool result = chunk.state.compare_exchange_strong(state, stateTo, std::memory_order_acq_rel);
        if (!result && actualValue != nullptr) *actualValue = state;
        return result;
    }
    bool addToChunkState(Chunk& chunk, int offset, int* actualValue = nullptr) {
        int state = chunk.state.load(std::memory_order_relaxed);
        if (state < 0 || state+offset < 0) {
            if (actualValue != nullptr) *actualValue = state;
            return false;
        }
        auto memoryOrder = offset > 0 ? std::memory_order_acquire : std::memory_order_release;
        while (!chunk.state.compare_exchange_strong(state, state+offset, memoryOrder)) {
            if (state < 0 || state+offset < 0) {
                if (actualValue != nullptr) *actualValue = state;
                return false;
            }
        }
        return true;
    }

    bool obtainListEndEditing() {
        bool modifying = _modifying_list_end.load(std::memory_order_relaxed);
        if (modifying) return false;
        return _modifying_list_end.compare_exchange_strong(modifying, true, std::memory_order_acquire);
    }
    void returnListEndEditing() {
        //assert(_modifying_list_end.load(std::memory_order_relaxed));
        _modifying_list_end.store(false, std::memory_order_release);
    }

    bool isClauseCompatible(const Clause& c) {
        return c.size == _clause_size && (!_uniform_lbd_values || c.lbd == _lbd_value);
    }

};

#endif
