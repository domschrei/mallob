
#pragma once

#include <atomic>
#include <limits>
#include <memory>
#include <cmath>
#include "app/sat/data/clause.hpp"
#include "app/sat/data/clause_histogram.hpp"
#include "app/sat/sharing/buffer/buffer_builder.hpp"
#include "app/sat/sharing/buffer/buffer_reader.hpp"
#include "util/logger.hpp"
#include "util/sys/threading.hpp"

class ClauseSlot {

private:
    const int _slot_idx;
    const int _clause_length;
    const int _common_lbd_or_zero;
    const int _effective_clause_length;

    std::atomic_int& _global_budget;

    std::atomic_int _nb_stored_clauses {0};

    Mutex _mtx;
    std::vector<int> _data;
    int _data_size {0};

    std::function<void(Mallob::Clause&)> _cb_discard_cls;

    std::vector<int> _tmp_clause_data;
    Mallob::Clause _tmp_clause;

    ClauseSlot* _left_neighbor {nullptr};

    ClauseHistogram* _hist_discarded_cls {nullptr};

public:
    ClauseSlot(std::atomic_int& globalBudget, int slotIdx, int clauseLength, int commonLbdOrZero) : 
        _global_budget(globalBudget), 
        _slot_idx(slotIdx), _clause_length(clauseLength), _common_lbd_or_zero(commonLbdOrZero),
        _effective_clause_length(hasIndividualLbds() ? _clause_length+1 : _clause_length), 
        _tmp_clause_data(_effective_clause_length), _tmp_clause(_tmp_clause_data.data(), _clause_length, _common_lbd_or_zero) {}

    void setLeftNeighbor(ClauseSlot* leftNeighbor) {
        _left_neighbor = leftNeighbor;
    }
    ClauseSlot* getLeftNeighbor() {
        return _left_neighbor;
    }

    void setDiscardedClausesNotification(std::function<void(Mallob::Clause&)> callback, ClauseHistogram& hist) {
        _cb_discard_cls = callback;
        _hist_discarded_cls = &hist;
    }

    bool insert(const Mallob::Clause& clause, ClauseSlot* maxNeighbor) {

        if (tryFetchBudget(1, maxNeighbor, false) == 0) 
            return false;
        // Budget fetched successfully!

        auto lock = _mtx.getLock();
        pushClauseToBack(clause);
        return true;
    }

    void insert(BufferReader& inputBuffer, ClauseSlot* maxNeighbor) {

        // fast-forward buffer reader to the first applicable slot
        auto clause = inputBuffer.getNextIncomingClause();
        while (clause.begin != nullptr && !fitsThisSlot(clause)) {
            clause = inputBuffer.getNextIncomingClause();
        }

        // There might be multiple slots in the buffer which are applicable for this slot.
        // Iterate over all of them.
        int budget = 0;
        while (clause.begin != nullptr && fitsThisSlot(clause)) {

            // Fetch number of clauses in the buffer slot, try and fetch budget accordingly
            int nbInputClauses = inputBuffer.getNumRemainingClausesInBucket();
            budget += tryFetchBudget(nbInputClauses, maxNeighbor, true);

            // Insert clauses one by one
            auto lock = _mtx.getLock();
            while (nbInputClauses > 0 && budget >= _clause_length) {
                pushClauseToBack(clause);
                budget -= _clause_length;
                nbInputClauses--;
                clause = inputBuffer.getNextIncomingClause();
            }
            if (budget < _clause_length) break;
        }

        // return excess budget
        if (budget > 0) storeBudget(budget);
    }

    bool pop(Mallob::Clause& clause) {
        return pop(clause, false);
    }

    bool popWeak(Mallob::Clause& clause) {
        return pop(clause, true);
    }

    void flushAndShrink(BufferBuilder& buf, std::function<void(int*)> clauseDataConverter) {

        // Acquire lock
        auto lock = _mtx.getLock();

        //std::string report;
        //for (size_t i = 0; i < _data_size; i++) report += std::to_string(_data[i]) + " ";
        //LOG(V2_INFO, "SLOT (%i,%i) BEFORE FLUSH: %s\n", _clause_length, _common_lbd_or_zero, report.c_str());

        // Fetch clauses as long as possible
        std::vector<Mallob::Clause> flushedClauses;
        int nbRemainingLits = buf.getMaxRemainingLits();
        nbRemainingLits -= (nbRemainingLits % _clause_length);
        int nbFreedLits = tryFreeStoredLiterals(nbRemainingLits, true);
        storeBudget(nbFreedLits);
        int dataIdx = _data_size - _effective_clause_length;
        while (nbFreedLits >= _clause_length) {
            // Clause freed - read, append to buffer, pop
            readClause(dataIdx, _tmp_clause);
            clauseDataConverter(_tmp_clause.begin);
            flushedClauses.push_back(_tmp_clause);
            nbFreedLits -= _clause_length;
            dataIdx -= _effective_clause_length;
        }
        assert(nbFreedLits == 0);
        std::sort(flushedClauses.begin(), flushedClauses.end());
        for (auto& cls : flushedClauses) {
            popFreedClauseAtBack();
            bool success = buf.append(cls);
            assert(success);
        }
        
        discardFreedClauses();

        // Shrink vector to fit actual data
        shrink();
    }

    // (called from another clause slot)
    // Try to remove some literals "virtually". The actual removal is done later
    // by the slot itself in the call "flushAndShrink".
    int tryFreeStoredLiterals(int nbDesiredLits, bool freePartially) {
        int nbClauses = _nb_stored_clauses.load(std::memory_order_relaxed);
        int nbDesiredCls = std::ceil(nbDesiredLits / (float)_clause_length);
        while (nbClauses > 0 &&
            !_nb_stored_clauses.compare_exchange_strong(nbClauses, std::max(0, nbClauses-nbDesiredCls), std::memory_order_relaxed)) {}
        
        int nbFreedLits = std::min(nbClauses, nbDesiredCls)*_clause_length;
        if (!freePartially && nbFreedLits < nbDesiredLits) {
            storeBudget(nbFreedLits);
            return 0;
        }
        if (nbFreedLits > nbDesiredLits) storeBudget(nbFreedLits);
        return nbFreedLits;
    }

    int getNbStoredLiterals() const {
        return _nb_stored_clauses.load(std::memory_order_relaxed) * _clause_length;
    }

private:

    void shrink() {
        _data.resize(_data_size);
        _data.shrink_to_fit();
    }

    // While the used budget count and the actual data_size are incoherent,
    // remove a clause from the back of the data and reduce data_size.
    void discardFreedClauses() {
        while (_data_size / _effective_clause_length > _nb_stored_clauses.load(std::memory_order_relaxed)) {
            readClauseAtBack(_tmp_clause);
            _cb_discard_cls(_tmp_clause);
            _hist_discarded_cls->increment(_clause_length);
            popFreedClauseAtBack();
        }
    }

    bool pop(Mallob::Clause& clause, bool giveUpOnLock) {

        // Is this slot empty? (Redundant check, but can avoid locking)
        if (_nb_stored_clauses.load(std::memory_order_relaxed) == 0) return false;

        // Acquire lock
        if (giveUpOnLock && !_mtx.tryLock()) return false;
        if (!giveUpOnLock) _mtx.lock();

        // Try to reduce the used budget by one clause.
        if (tryFreeStoredLiterals(_clause_length, false) == 0) {
            _mtx.unlock();
            return false;
        }
        
        // Success! At least one clause is here and can be popped.
        readClauseAtBack(clause);
        popFreedClauseAtBack();

        // Now that you are owning the lock, also discard clauses
        // which were marked to be discarded.
        discardFreedClauses();

        _mtx.unlock();
        return true;
    }

    void pushClauseToBack(const Mallob::Clause& clause) {
        _nb_stored_clauses.fetch_add(1);
        if (_data.size() < _data_size + _effective_clause_length)
            _data.resize(_data_size + _effective_clause_length);
        auto pos = _data_size;
        _data_size += _effective_clause_length;

        if (hasIndividualLbds()) _data[pos++] = clause.lbd;
        for (size_t i = 0; i < clause.size; ++i) _data[pos++] = clause.begin[i];
    }

    void readClauseAtBack(Mallob::Clause& clause) {
        return readClause(_data_size - _effective_clause_length, clause);
    }

    void readClause(int dataIdx, Mallob::Clause& clause) {
        clause.size = _clause_length;
        int* clsData = _data.data() + dataIdx;
        if (hasIndividualLbds()) clause.lbd = *(clsData++);
        else clause.lbd = _common_lbd_or_zero;
        clause.begin = clsData;
    }

    void popFreedClauseAtBack() {
        assert(_data_size >= _effective_clause_length);
        _data_size -= _effective_clause_length;
    }

    int tryFetchBudget(int nbClauses, ClauseSlot* maxNeighbor, bool fetchPartially) {

        int nbDesiredLits = nbClauses*_clause_length;

        // Fetch budget from global budget
        int budget = fetchBudget(nbDesiredLits);
        // Insufficient?
        if (budget < nbDesiredLits) {
            // Try fetch budget from other slots
            ClauseSlot* neighbor = maxNeighbor;
            while (neighbor != this && budget < nbDesiredLits) {
                budget += neighbor->tryFreeStoredLiterals(nbDesiredLits-budget, true);
                neighbor = neighbor->getLeftNeighbor();
            }
            // Still insufficient?
            if (budget < nbDesiredLits) {
                // Return partial budget if desired
                if (fetchPartially) return budget;
                // Return budget remainder (if any)
                if (budget > 0) storeBudget(budget);
                return 0;
            }
        }
        // Return excess budget
        if (budget > nbDesiredLits) {
            storeBudget(budget-nbDesiredLits);
        }
        return true;
    }

    int fetchBudget(int numDesired) {
        int globalBudget = _global_budget.load(std::memory_order_relaxed);
        while (globalBudget > 0) {
            int amountToFetch = std::min(globalBudget, numDesired);
            if (_global_budget.compare_exchange_strong(globalBudget, globalBudget-amountToFetch, 
                std::memory_order_relaxed)) {
                // success
                return amountToFetch;
            }
        }
        return 0;
    }

    void storeBudget(int amount) {
        _global_budget.fetch_add(amount, std::memory_order_relaxed);
    }

    bool fitsThisSlot(Mallob::Clause& clause) {
        return clause.size == _clause_length && 
            (hasIndividualLbds() || clause.lbd == _common_lbd_or_zero);
    }

    bool hasIndividualLbds() const {
        return _common_lbd_or_zero==0;
    }
};
