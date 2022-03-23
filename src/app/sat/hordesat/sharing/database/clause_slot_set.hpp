
#pragma once

#include "produced_clause.hpp"
#include "util/sys/atomics.hpp"
#include "util/sys/threading.hpp"
#include "util/logger.hpp"

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
        if (!acquired) {

            // No success: Try to steal memory from a less important slot
            numFreedLits = _free_low_priority_literals();
            
            // Freed enough memory?
            if (numFreedLits >= len) {
                // Steal successful: use len freed lits for this clause implicitly
                acquired = true;
                // Release the freed budget which you DO NOT need for this clause insertion
                if (numFreedLits != len)
                    _literals_budget.fetch_add(numFreedLits-len, std::memory_order_relaxed);
                
            } else if (numFreedLits > 0) {
                // Return insufficient freed budget to global storage
                _literals_budget.fetch_add(numFreedLits, std::memory_order_relaxed);
            }
            
            // Retry acquisition explicitly, if necessary
            acquired = acquired || tryAcquireBudget(len);
            if (!acquired) {
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
            //int maxNonemptySlot = _max_nonempty_slot.load(std::memory_order_relaxed);
            //while (maxNonemptySlot < _slot_idx && !_max_nonempty_slot.compare_exchange_strong(
            //    maxNonemptySlot, _slot_idx, std::memory_order_relaxed)) {}
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

    int free(int numLiterals) {

        if (_empty.load(std::memory_order_relaxed)) {
            //LOG(V2_INFO, "%i ALREADY EMPTY\n", _slot_idx);
            return 0;
        }

        _set_mutex.lock();

        // Drop clauses while possible and while within desired number of freed literals
        int numFreedLiterals = 0;
        while (numFreedLiterals < numLiterals && dropClause(numFreedLiterals)) {}

        _set_mutex.unlock();

        return numFreedLiterals;
    }

    ClauseSlotMode getSlotMode() const {return _slot_mode;}

    bool tryDecreaseNonemptySlotIdx(bool lockHeld) {

        if (!lockHeld && !_set_mutex.tryLock()) return false;

        bool success = false;
        bool empty = _set.empty();
        if (empty) {
            int maxNonemptySlot = _max_nonempty_slot.load(std::memory_order_relaxed);
            while (maxNonemptySlot == _slot_idx && !_max_nonempty_slot.compare_exchange_strong(
                maxNonemptySlot, _slot_idx-1)) {}
            success = _max_nonempty_slot.load(std::memory_order_relaxed) < _slot_idx;
        }

        if (!lockHeld) _set_mutex.unlock();
        //LOG(V2_INFO, "%i : TRYDECREASE:%i (mnes=%i empty=%i)\n", _slot_idx, success?1:0, (int)_max_nonempty_slot, empty?1:0);
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

        } else if (_last_it == end) {
            _last_it = _set.begin();
        }
        
        return clause;
    }

    inline bool dropClause(int& droppedLiterals) {

        auto end = _set.end();
        if (_last_it == end) {
            _empty.store(true, std::memory_order_relaxed);
            return false;
        }
        
        auto size = prod_cls::size(_last_it.key());
        _last_it = _set.erase(_last_it);
        droppedLiterals += size;

        if (_set.empty()) {
            // Slot just became empty
            _empty.store(true, std::memory_order_relaxed);

        } else if (_last_it == end) {
            _last_it = _set.begin();
        }

        return true;
    }

    bool tryAcquireBudget(int len) {
        int budget = _literals_budget.load(std::memory_order_relaxed);
        while (budget >= len && !_literals_budget.compare_exchange_strong(budget, budget-len, std::memory_order_relaxed)) {}
        return budget >= len;
    }
};
