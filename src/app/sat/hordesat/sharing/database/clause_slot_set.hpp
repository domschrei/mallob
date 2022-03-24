
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
    
    Mutex _map_mutex;
    ProducedClauseMap<T> _map;
    typename ProducedClauseMap<T>::iterator _last_it;

    std::function<int()> _free_low_priority_literals;

public:

    ClauseSlotSet(int slotIdx, ClauseSlotMode mode, std::atomic_int& literalsBudget, std::atomic_int& maxNonemptySlot,
            std::function<int()> freeLowPriorityLiterals) : 
        _slot_idx(slotIdx), _slot_mode(mode), _literals_budget(literalsBudget), 
        _max_nonempty_slot(maxNonemptySlot), _free_low_priority_literals(freeLowPriorityLiterals) {

        _last_it = _map.end();
    }

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
        T producedClause(c);
        assert(producedClause.valid());

        auto lock = _map_mutex.getLock();

        // Query if clause is already present
        auto it = _map.find(producedClause);
        bool alreadyPresent = it != _map.end();
        uint32_t producerFlag = 1 << producerId;
        
        // Clause already contained?
        if (alreadyPresent) {
            // -- yes: add additional producer, return budget, return failure
            it.value() |= producerFlag;
            _literals_budget.fetch_add(len, std::memory_order_relaxed);
            return DUPLICATE;
        } else {
            // -- no: just insert
            _last_it = _map.insert({std::move(producedClause), producerFlag}).first;
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

    T pop(int literalLimit, uint32_t& producers, bool allowSpuriousFailure = false) {

        if (_empty.load(std::memory_order_relaxed)) return T();

        if (allowSpuriousFailure && !_map_mutex.tryLock()) return T();
        if (!allowSpuriousFailure) _map_mutex.lock();

        T clause = popUnsafe(literalLimit, producers);

        _map_mutex.unlock();

        return clause;
    }

    T popUnsafe(int literalLimit, uint32_t& producers) {
        T clause = popClause(literalLimit, producers);
        if (clause.valid()) {
            int len = prod_cls::size(clause);
            _literals_budget.fetch_add(len, std::memory_order_relaxed);
        }
        return clause;
    }

    int free(int numLiterals) {

        if (_empty.load(std::memory_order_relaxed)) {
            //LOG(V2_INFO, "%i ALREADY EMPTY\n", _slot_idx);
            return 0;
        }

        _map_mutex.lock();

        // Drop clauses while possible and while within desired number of freed literals
        int numFreedLiterals = 0;
        while (numFreedLiterals < numLiterals && dropClause(numFreedLiterals)) {}

        _map_mutex.unlock();

        return numFreedLiterals;
    }

    void acquireLock() {_map_mutex.lock();}
    void releaseLock() {_map_mutex.unlock();}

    ClauseSlotMode getSlotMode() const {return _slot_mode;}

private:

    inline T popClause(int totalLiteralLimit, uint32_t& producers) {
        
        auto end = _map.end();
        if (_last_it == end) {
            _empty.store(true, std::memory_order_relaxed);
            return T();
        }
        
        if (prod_cls::size(_last_it.key()) > totalLiteralLimit) return T();
        
        producers = _last_it.value();
        T clause = _last_it.key().extractUnsafe();
        _last_it = _map.erase(_last_it);

        if (_map.empty()) {
            // Slot just became empty
            _empty.store(true, std::memory_order_relaxed);

        } else if (_last_it == end) {
            _last_it = _map.begin();
        }
        
        return clause;
    }

    inline bool dropClause(int& droppedLiterals) {

        auto end = _map.end();
        if (_last_it == end) {
            _empty.store(true, std::memory_order_relaxed);
            return false;
        }
        
        auto size = prod_cls::size(_last_it.key());
        _last_it = _map.erase(_last_it);
        droppedLiterals += size;

        if (_map.empty()) {
            // Slot just became empty
            _empty.store(true, std::memory_order_relaxed);

        } else if (_last_it == end) {
            _last_it = _map.begin();
        }

        return true;
    }

    bool tryAcquireBudget(int len) {
        int budget = _literals_budget.load(std::memory_order_relaxed);
        while (budget >= len && !_literals_budget.compare_exchange_strong(budget, budget-len, std::memory_order_relaxed)) {}
        return budget >= len;
    }
};
