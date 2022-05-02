
#pragma once

#include <forward_list>
#include <memory>
#include <numeric>


#include "../../data/produced_clause.hpp"
#include "bucket_label.hpp"
#include "buffer_reader.hpp"
#include "buffer_merger.hpp"
#include "util/periodic_event.hpp"
#include "../../data/solver_statistics.hpp"

/*
Buffering structure for clauses which sorts and prioritizes clauses 
by length (primary) and LBD score (secondary). The structure is adaptive
because memory chunks of fixed size are allocated on demand and
can be moved freely from one length-LBD slot to another as necessary.
*/
class AdaptiveClauseDatabase {

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

    int _total_literal_limit;
    std::atomic_int _nb_used_literals {0};

    template <typename T>
    struct Slot {
        int implicitLbdOrZero;
        std::atomic_int nbLiterals {0};
        std::atomic_int freeLocalBudget {0};
        std::shared_ptr<Mutex> mtx;
        std::forward_list<T> list;
        Slot() = default;
        Slot(Slot&& other) :
            implicitLbdOrZero(other.implicitLbdOrZero),
            freeLocalBudget(other.freeLocalBudget.load(std::memory_order_relaxed)), 
            nbLiterals(other.nbLiterals.load(std::memory_order_relaxed)), 
            mtx(std::move(other.mtx)),
            list(other.list) {}
    };

    Slot<int> _unit_slot;
    Slot<std::pair<int, int>> _binary_slot;
    std::vector<Slot<std::vector<int>>> _large_slots;
    
    enum ClauseSlotMode {SAME_SUM_OF_SIZE_AND_LBD, SAME_SIZE, SAME_SIZE_AND_LBD};
    robin_hood::unordered_flat_map<std::pair<int, int>, std::pair<int, ClauseSlotMode>, IntPairHasher> _size_lbd_to_slot_idx_mode;

    int _max_lbd_partitioned_size;
    int _max_clause_length;
    bool _slots_for_sum_of_length_and_lbd;

    bool _use_checksum;
    BucketLabel _bucket_iterator;

    ClauseHistogram _hist_deleted_in_slots;

public:
    struct Setup {
        int numLiterals = 1000;
        int maxClauseLength = 20;
        int maxLbdPartitionedSize = 2;
        bool useChecksums = false;
        bool slotsForSumOfLengthAndLbd = false;
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

    int reserveLiteralBudget(int cSize, int cLbd);

    int getLocalBudget(int slotIdx) const {
        if (slotIdx == -2) {
            return _unit_slot.freeLocalBudget.load(std::memory_order_relaxed);
        } else if (slotIdx == -1) {
            return _binary_slot.freeLocalBudget.load(std::memory_order_relaxed);
        } else {
            return _large_slots[slotIdx].freeLocalBudget.load(std::memory_order_relaxed);
        }
    }
    void storeLocalBudget(int slotIdx, int amount) {
        if (slotIdx == -2) {
            _unit_slot.freeLocalBudget.fetch_add(amount, std::memory_order_relaxed);
        } else if (slotIdx == -1) {
            _binary_slot.freeLocalBudget.fetch_add(amount, std::memory_order_relaxed);
        } else {
            _large_slots[slotIdx].freeLocalBudget.fetch_add(amount, std::memory_order_relaxed);
        }
    }
    void storeGlobalBudget(int amount) {
        _large_slots.back().freeLocalBudget.fetch_add(amount, std::memory_order_relaxed);
    }

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

        atomics::addRelaxed(_nb_used_literals, nbLiterals);
        float timeInsert = Timer::elapsedSeconds();
        T& clause = clauses.front();

        if constexpr (std::is_same<T, int>::value) {
            auto lock = _unit_slot.mtx->getLock();
            _unit_slot.list.splice_after(_unit_slot.list.before_begin(), clauses);
            atomics::addRelaxed(_unit_slot.nbLiterals, nbLiterals);
            assert_heavy(checkNbLiterals(_unit_slot));
        } else if constexpr (std::is_same<T, std::pair<int, int>>::value) {
            auto lock = _binary_slot.mtx->getLock();
            _binary_slot.list.splice_after(_binary_slot.list.before_begin(), clauses);
            atomics::addRelaxed(_binary_slot.nbLiterals, nbLiterals);
            assert_heavy(checkNbLiterals(_binary_slot));
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
            auto lock = slot.mtx->getLock();
            slot.list.splice_after(slot.list.before_begin(), clauses);
            atomics::addRelaxed(slot.nbLiterals, nbLiterals);
            assert_heavy(checkNbLiterals(slot));
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
        return _nb_used_literals.load(std::memory_order_relaxed);
    }
    int getNumLiterals(int clauseLength, int lbd) {
        if (clauseLength == 1) return _unit_slot.nbLiterals.load(std::memory_order_relaxed);
        if (clauseLength == 2) return _binary_slot.nbLiterals.load(std::memory_order_relaxed);
        auto [slotIdx, mode] = getSlotIdxAndMode(clauseLength, lbd);
        return _large_slots[slotIdx].nbLiterals.load(std::memory_order_relaxed);
    }

    ClauseHistogram& getDeletedClausesHistogram();

    bool checkTotalLiterals() {

        assert(checkNbLiterals(_unit_slot));
        assert(checkNbLiterals(_binary_slot));

        int nbUsedAdvertised = _nb_used_literals;
        
        int nbUsedActual = _unit_slot.nbLiterals + _binary_slot.nbLiterals;
        int foundBudget = _unit_slot.freeLocalBudget + _binary_slot.freeLocalBudget;
        for (auto& slot : _large_slots) {
            assert(checkNbLiterals(slot));
            nbUsedActual += slot.nbLiterals;
            foundBudget += slot.freeLocalBudget;
        }
        
        assert(nbUsedAdvertised == nbUsedActual || 
            log_return_false("Mismatch in used literals: %i advertised, %i actual\n", nbUsedAdvertised, nbUsedActual));
        assert(_nb_used_literals + foundBudget == _total_literal_limit || 
            log_return_false("Mismatch in literal budget: %i used, total local budget %i, total literal limit %i\n", 
            _nb_used_literals.load(), foundBudget, _total_literal_limit)
        );
        return true;
    }

private:
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
    void flushClauses(Slot<T>& slot, bool sortClauses, BufferBuilder& builder);
    
    std::pair<int, ClauseSlotMode> getSlotIdxAndMode(int clauseSize, int lbd);
    BucketLabel getBucketIterator();

    int tryAcquireBudget(int callingSlot, int desiredLits, float freeingFactor = 1.0f);
};
