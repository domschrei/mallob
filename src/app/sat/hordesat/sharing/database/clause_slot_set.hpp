
#pragma once

#include "produced_clause.hpp"
#include "util/sys/atomics.hpp"
#include "util/sys/threading.hpp"

enum ClauseSlotMode {SAME_SUM_OF_SIZE_AND_LBD, SAME_SIZE, SAME_SIZE_AND_LBD};
enum ClauseSlotInsertResult {SUCCESS, NO_BUDGET, DUPLICATE};

template<typename T>
class ClauseSlotSet {

private:
    int _slot_idx;
    ClauseSlotMode _slot_mode;
    int _max_slot;
    
    std::atomic_int& _literals_budget;
    std::atomic_int& _max_nonempty_slot;
    std::atomic_bool _empty {true};
    
    Mutex _set_mutex;
    ProducedClauseSet<T> _set;
    typename ProducedClauseSet<T>::iterator _last_it;

    std::function<int()> _free_low_priority_literals;

public:

    ClauseSlotSet(int slotIdx, ClauseSlotMode mode, std::atomic_int& literalsBudget, std::atomic_int& maxNonemptySlot,
            std::function<int()> freeLowPriorityLiterals) : 
        _slot_idx(slotIdx), _slot_mode(mode), _literals_budget(literalsBudget), 
        _max_nonempty_slot(maxNonemptySlot), _free_low_priority_literals(freeLowPriorityLiterals) {}

    void setMaxSlotIdx(int maxSlotIdx) {_max_slot = maxSlotIdx;}

    ClauseSlotInsertResult insert(const Mallob::Clause& c, int producerId) {
        
        assert(c.begin != nullptr);
        int len = c.size;

        // Try to acquire budget
        bool acquired = tryAcquireBudget(len);
        int numFreedLits = 0;
        while (!acquired) {

            // No success: Try to steal memory from a less important slot
            numFreedLits = _free_low_priority_literals();
            
            // Freed some amount of memory?
            if (numFreedLits >= len) {
                // Steal successful: use len freed lits for this clause implicitly
                acquired = true;
                // Release the freed budget which you DO NOT need for this clause insertion
                _literals_budget.fetch_add(numFreedLits-len, std::memory_order_relaxed);
                break;
            } else if (numFreedLits > 0) {
                // Return insufficient freed budget to global storage
                _literals_budget.fetch_add(numFreedLits, std::memory_order_relaxed);
            }
            
            // Retry acquisition explicitly
            acquired = tryAcquireBudget(len);
            if (numFreedLits == 0 && !acquired) {
                // No success at all : There is no memory left to free - 
                // this clause is not important enough to be stored right now. Discard it.
                return NO_BUDGET;
            }
        }

        // Budget has been acquired successfully: create clause
        T producedClause(c, producerId);
        assert(producedClause.valid());

        auto lock = _set_mutex.getLock();

        // Query if clause is already present
        auto it = _set.find(producedClause);
        bool alreadyPresent = it != _set.end();
        
        // Clause already contained?
        if (alreadyPresent) {
            // -- yes: erase and re-insert with additional producer, return budget, return failure
            producedClause.producers |= it.key().producers;
            it = _set.erase(it);
            _last_it = _set.emplace_hint(it, std::move(producedClause));
            _literals_budget.fetch_add(len, std::memory_order_relaxed);
            return DUPLICATE;
        } else {
            // -- no: just insert
            _last_it = _set.emplace(std::move(producedClause)).first;
        }

        // Clause was inserted successfully

        if (_empty.load(std::memory_order_relaxed)) {
            // Slot was empty and became non-empty
            _empty.store(false, std::memory_order_relaxed);
            
            // If current max. nonempty slot is lower than this slot, update it
            int maxNonemptySlot = _max_nonempty_slot.load(std::memory_order_relaxed);
            while (maxNonemptySlot < _slot_idx && !_max_nonempty_slot.compare_exchange_strong(
                maxNonemptySlot, _slot_idx, std::memory_order_relaxed)) {}
        }

        return SUCCESS;
    }

    T pop(bool allowSpuriousFailure = false) {

        if (_empty.load(std::memory_order_relaxed)) return T();

        if (allowSpuriousFailure && !_set_mutex.tryLock()) return T();
        if (!allowSpuriousFailure) _set_mutex.lock();

        T clause = popClause(INT32_MAX);

        _set_mutex.unlock();

        if (clause.valid()) {
            int len = prod_cls::size(clause);
            _literals_budget.fetch_add(len, std::memory_order_relaxed);
        }

        return clause;
    }

    std::vector<T> flush(int totalLiteralLimit) {
        
        std::vector<T> flushedClauses;
        if (_empty.load(std::memory_order_relaxed)) return flushedClauses;

        if (totalLiteralLimit < 0) totalLiteralLimit = INT32_MAX;

        _set_mutex.lock();

        int numIterations = 0;
        while (true) {
        
            // yield lock from time to time
            numIterations++;
            if (numIterations % 16 == 0) {
                _set_mutex.unlock();
                _set_mutex.lock();
            }

            T clause = popClause(totalLiteralLimit);
            if (!clause.valid()) break;

            int len = prod_cls::size(clause);
            flushedClauses.push_back(std::move(clause));
            
            _literals_budget.fetch_add(len, std::memory_order_relaxed);
            totalLiteralLimit -= len;
        }

        _set_mutex.unlock();

        return flushedClauses;
    }

    int free(int numLiterals, bool& emptyAfterCall) {

        if (_empty.load(std::memory_order_relaxed)) {
            emptyAfterCall = true;
            return 0;
        }

        _set_mutex.lock();

        int numFreedLiterals = 0;
        int numIterations = 0;
        emptyAfterCall = false;
        while (numFreedLiterals < numLiterals) {
                    
            // yield lock from time to time
            numIterations++;
            if (numIterations % 16 == 0) {
                _set_mutex.unlock();
                _set_mutex.lock();
            }

            T clause = popClause(INT32_MAX);
            if (!clause.valid()) {
                emptyAfterCall = true;
                break;
            }
            
            int len = prod_cls::size(clause);
            numFreedLiterals += len;
        }

        _set_mutex.unlock();

        return numFreedLiterals;
    }

    ClauseSlotMode getSlotMode() const {return _slot_mode;}

    bool tryDecreaseNonemptySlotIdx() {

        if (!_set_mutex.tryLock()) return false;

        bool success = false;
        if (_set.empty()) {
            int maxNonemptySlot = _max_nonempty_slot.load(std::memory_order_relaxed);
            while (maxNonemptySlot == _slot_idx && !_max_nonempty_slot.compare_exchange_strong(
                maxNonemptySlot, _max_slot-1)) {}
            if (maxNonemptySlot == _slot_idx) success = true;
        }

        _set_mutex.unlock();
        return success;
    }

private:

    inline T popClause(int totalLiteralLimit) {
        
        auto end = _set.end();
        if (_last_it == end) {
            _empty.store(true, std::memory_order_relaxed);
            return T();
        }
        
        if (prod_cls::size(_last_it.key()) > totalLiteralLimit) return T();
        
        T clause = _last_it.key().extractUnsafe();
        _last_it = _set.erase(_last_it);

        if (_set.empty()) {
            // Slot just became empty
            _empty.store(true, std::memory_order_relaxed);

            // If max. nonempty slot is THIS slot, decrease the max. nonempty slot index
            // conservatively by one.
            int maxNonemptySlot = _max_nonempty_slot.load(std::memory_order_relaxed);
            while (maxNonemptySlot == _slot_idx && !_max_nonempty_slot.compare_exchange_strong(maxNonemptySlot, _max_slot-1)) {}

        } else if (_last_it == end) {
            _last_it = _set.begin();
        }
        
        return clause;
    }

    bool tryAcquireBudget(int len) {
        int budget = _literals_budget.load(std::memory_order_relaxed);
        while (budget >= len && !_literals_budget.compare_exchange_strong(budget, budget-len, std::memory_order_relaxed)) {}
        return budget >= len;
    }
};
