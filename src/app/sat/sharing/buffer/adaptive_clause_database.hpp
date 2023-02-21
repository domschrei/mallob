
#pragma once

#include <atomic>
#include <forward_list>
#include <limits>
#include <memory>
#include <numeric>


#include "../../data/produced_clause.hpp"
#include "bucket_label.hpp"
#include "buffer_reader.hpp"
#include "buffer_merger.hpp"
#include "util/logger.hpp"
#include "util/periodic_event.hpp"
#include "../../data/solver_statistics.hpp"

/*
Buffering structure for clauses which sorts and prioritizes clauses 
by length (primary) and LBD score (secondary). The structure is adaptive
because memory budget can be transferred freely from one length-LBD slot
to another as necessary.
*/
class AdaptiveClauseDatabase {

private:
    template <typename T>
    struct Slot {
        int implicitLbdOrZero;
        std::atomic_int nbLiterals {0};
        std::shared_ptr<Mutex> mtx;
        std::forward_list<T> list;
        Slot() = default;
        Slot(Slot&& other) :
            implicitLbdOrZero(other.implicitLbdOrZero),
            nbLiterals(other.nbLiterals.load(std::memory_order_relaxed)), 
            mtx(std::move(other.mtx)),
            list(other.list) {}
        void lock() {
            if (mtx) mtx->lock();
        }
        bool tryLock() {
            if (!mtx) return true;
            return mtx->tryLock();
        }
        void unlock() {
            if (mtx) mtx->unlock();
        }
    };

    int _total_literal_limit;
    Slot<int> _unit_slot;
    Slot<std::pair<int, int>> _binary_slot;
    std::vector<Slot<std::vector<int>>> _large_slots;
    std::atomic_int _free_budget {0};
    std::atomic_int _max_admissible_cls_size {std::numeric_limits<int>::max()};

    enum ClauseSlotMode {SAME_SUM_OF_SIZE_AND_LBD, SAME_SIZE, SAME_SIZE_AND_LBD};
    robin_hood::unordered_flat_map<std::pair<int, int>, std::pair<int, ClauseSlotMode>, IntPairHasher> _size_lbd_to_slot_idx_mode;

    int _max_lbd_partitioned_size;
    int _max_clause_length;
    bool _slots_for_sum_of_length_and_lbd;

    bool _use_checksum;
    BucketLabel _bucket_iterator;

    ClauseHistogram _hist_deleted_in_slots;

    bool _has_cb_clause_deleted {false};
    std::function<void(Mallob::Clause&)> _cb_clause_deleted;

public:
    struct Setup {
        int numLiterals = 1000;
        int maxClauseLength = 20;
        int maxLbdPartitionedSize = 2;
        bool useChecksums = false;
        bool slotsForSumOfLengthAndLbd = false;
        bool threadSafe = true;
    };

    // Use to consecutively insert clauses into the database.
    // For each size-LBD group, the available literal budget can be queried
    // based on the state of the previous groups.
    struct LinearBudgetCounter {
        AdaptiveClauseDatabase* cdb {nullptr};
        int intermediateBudget {0};
        int lastCountedSlotIdx {-3};
        LinearBudgetCounter() {}
        LinearBudgetCounter(AdaptiveClauseDatabase& cdb) : cdb(&cdb), 
            intermediateBudget(cdb.getTotalLiteralBudget()), lastCountedSlotIdx(-3) {}
        int getNextBudget(int nbPrevUsed, int size, int lbd) {
            intermediateBudget -= nbPrevUsed;
            int slotIdx = cdb->getSlotIdxAndMode(size, lbd).first;
            assert(slotIdx >= lastCountedSlotIdx);
            while (lastCountedSlotIdx < slotIdx) {
                lastCountedSlotIdx++;
                if (lastCountedSlotIdx == -2) intermediateBudget -= cdb->_unit_slot.nbLiterals;
                else if (lastCountedSlotIdx == -1) intermediateBudget -= cdb->_binary_slot.nbLiterals;
                else intermediateBudget -= cdb->_large_slots[lastCountedSlotIdx].nbLiterals;
            }
            return intermediateBudget;
        }
    };

    AdaptiveClauseDatabase(Setup setup);
    ~AdaptiveClauseDatabase() {}

    /*
    Insert a clause from a certain producer (0 <= ID < #producers).
    true: The clause has been inserted.
    false: The clause has been rejected due to lack of space relative 
    to the clause's importance and can be discarded (although it may 
    be possible to insert it later).
    In both cases, c can be freed or reused after calling this method.
    */
    bool addClause(const Clause& c, bool sortLargeClause = false);
    bool addClause(int* cBegin, int cSize, int cLbd, bool sortLargeClause = false);

    template <typename T>
    void addReservedUniformClauses(int cSize, int cLbd, std::forward_list<T>& clauses, int nbLiterals) {
        
        float timeFree = Timer::elapsedSeconds();
        auto [slotIdx, mode] = getSlotIdxAndMode(cSize, cLbd);
        int freed = tryAcquireBudget(slotIdx, nbLiterals);
        assert(freed >= nbLiterals);
        if (freed > nbLiterals) {
            // return excess
            storeGlobalBudget(freed-nbLiterals);
        }
        timeFree = Timer::elapsedSeconds() - timeFree;

        float timeInsert = Timer::elapsedSeconds();
        T& clause = clauses.front();

        if constexpr (std::is_same<T, int>::value) {
            _unit_slot.lock();
            _unit_slot.list.splice_after(_unit_slot.list.before_begin(), clauses);
            atomics::addRelaxed(_unit_slot.nbLiterals, nbLiterals);
            assert_heavy(checkNbLiterals(_unit_slot));
            _unit_slot.unlock();
        } else if constexpr (std::is_same<T, std::pair<int, int>>::value) {
            _binary_slot.lock();
            _binary_slot.list.splice_after(_binary_slot.list.before_begin(), clauses);
            atomics::addRelaxed(_binary_slot.nbLiterals, nbLiterals);
            assert_heavy(checkNbLiterals(_binary_slot));
            _binary_slot.unlock();
        } else if constexpr (std::is_same<T, std::vector<int>>::value) {
            auto& slot = _large_slots[slotIdx];
            if (slot.implicitLbdOrZero == 0) {
                // Explicit LBD
                assert(clause.size() == cSize+1);
                assert(clause[0] == cLbd);
            } else {
                // Implicit LBD
                assert(clause.size() == cSize);
            }
            slot.lock();
            slot.list.splice_after(slot.list.before_begin(), clauses);
            atomics::addRelaxed(slot.nbLiterals, nbLiterals);
            assert_heavy(checkNbLiterals(slot));
            slot.unlock();
        }
        timeInsert = Timer::elapsedSeconds() - timeInsert;

        LOG(V6_DEBGV, "DG (%i,%i) %.4fs free, %.4fs insert\n", cSize, cLbd, timeFree, timeInsert);
    }

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
            ExportMode mode = ANY, bool sortClauses = true, 
            std::function<void(int*)> clauseDataConverter = [](int*){});

    std::vector<int> exportBufferWithoutDeletion(int sizeLimit, int& numExportedClauses,
            ExportMode mode = ANY, bool sortClauses = true);

    bool popFrontWeak(ExportMode mode, Mallob::Clause& out);

    /*
    Allows to iterate over the clauses contained in a flat vector of integers
    as exported by exportBuffer. Must have been created by an AdaptiveClauseDatabase
    or a BufferMerger with the same parametrization as this instance.
    Throughout the life time of the BufferReader, the underlying vector must be valid.
    */
    BufferReader getBufferReader(int* begin, size_t size, bool useChecksums = false);
    BufferMerger getBufferMerger(int sizeLimit);
    BufferBuilder getBufferBuilder(std::vector<int>* out = nullptr);

    int getCurrentlyUsedLiterals() const {
        return _total_literal_limit - _free_budget.load(std::memory_order_relaxed);
    }
    int getNumLiterals(int clauseLength, int lbd) {
        if (clauseLength == 1) return _unit_slot.nbLiterals.load(std::memory_order_relaxed);
        if (clauseLength == 2) return _binary_slot.nbLiterals.load(std::memory_order_relaxed);
        auto [slotIdx, mode] = getSlotIdxAndMode(clauseLength, lbd);
        return _large_slots[slotIdx].nbLiterals.load(std::memory_order_relaxed);
    }
    int getTotalLiteralBudget() const {
        return _total_literal_limit;
    }

    ClauseHistogram& getDeletedClausesHistogram();

    bool checkTotalLiterals() {

        assert(checkNbLiterals(_unit_slot));
        assert(checkNbLiterals(_binary_slot));

        int nbUsedAdvertised = getCurrentlyUsedLiterals();
        
        int nbUsedActual = _unit_slot.nbLiterals + _binary_slot.nbLiterals;
        int foundBudget = _free_budget.load(std::memory_order_relaxed);
        for (auto& slot : _large_slots) {
            assert(checkNbLiterals(slot));
            nbUsedActual += slot.nbLiterals;
        }
        
        assert(nbUsedAdvertised == nbUsedActual || 
            log_return_false("Mismatch in used literals: %i advertised, %i actual\n", nbUsedAdvertised, nbUsedActual));
        assert(nbUsedAdvertised + foundBudget == _total_literal_limit || 
            log_return_false("Mismatch in literal budget: %i used, total local budget %i, total literal limit %i\n", 
            nbUsedAdvertised, foundBudget, _total_literal_limit)
        );
        return true;
    }

    void setClauseDeletionCallback(std::function<void(Mallob::Clause&)> cb) {
        _has_cb_clause_deleted = true;
        _cb_clause_deleted = cb;
    }

    void clearClauseDeletionCallback() {
        _has_cb_clause_deleted = false;
    }

private:

    int fetchGlobalBudget(int numDesired) {
        int globalBudget = _free_budget.load(std::memory_order_relaxed);
        while (globalBudget > 0) {
            int amountToFetch = std::min(globalBudget, numDesired);
            if (_free_budget.compare_exchange_strong(globalBudget, globalBudget-amountToFetch, 
                std::memory_order_relaxed)) {
                // success
                return amountToFetch;
            }
        }
        return 0;
    }
    void storeGlobalBudget(int amount) {
        _free_budget.fetch_add(amount, std::memory_order_relaxed);
    }

    template <typename T>
    bool checkNbLiterals(Slot<T>& slot, std::string additionalInfo = "") {

        int nbAdvertised = slot.nbLiterals.load(std::memory_order_relaxed);
        int nbActual = 0;
        for (auto elem : slot.list) {
            if constexpr (std::is_same<int, T>::value) {
                nbActual++;
            }
            if constexpr (std::is_same<std::pair<int, int>, T>::value) {
                nbActual += 2;
            }
            if constexpr (std::is_same<std::vector<int>, T>::value) {
                nbActual += elem.size() - (slot.implicitLbdOrZero==0 ? 1:0);
            }
        }
        if (nbAdvertised != nbActual) 
            LOG(V0_CRIT, "[ERROR] Slot advertised %i literals - found %i literals (%s)\n", 
                nbAdvertised, nbActual, additionalInfo.c_str());
        return nbAdvertised == nbActual;
    }

    template <typename T>
    bool popMallobClause(Slot<T>& slot, bool giveUpOnLock, Mallob::Clause& out);

    template <typename T>
    Mallob::Clause getMallobClause(T& elem, int implicitLbdOrZero);

    template <typename T>
    int stealBudgetFromSlot(Slot<T>& slot, int desiredLiterals, bool dropClauses);

    template <typename T>
    void flushClauses(Slot<T>& slot, bool sortClauses, BufferBuilder& builder, 
            std::function<void(int*)> clauseDataConverter = [](int*){});
    
    template <typename T>
    void readClauses(Slot<T>& slot, bool sortClauses, BufferBuilder& builder);
    
    std::pair<int, ClauseSlotMode> getSlotIdxAndMode(int clauseSize, int lbd);
    BucketLabel getBucketIterator();

    int tryAcquireBudget(int callingSlot, int desiredLits, float freeingFactor = 1.0f);
};
