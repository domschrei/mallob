
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

    // Reduces global budget by clause.size
    // AND/OR
    // reduces stored clauses in another slot and may store remainder in global budget
    bool insert(const Mallob::Clause& clause, ClauseSlot* maxNeighbor) {
        assert(clause.size == _clause_length);

        int fetchedBudget = tryFetchBudget(1, maxNeighbor, false);
        //LOG(V2_INFO, "(%i,%i) FETCHED_BUDGET=%i\n", _clause_length, _common_lbd_or_zero, fetchedBudget);
        if (fetchedBudget == 0)
            return false;
        // Budget fetched successfully!
        assert(fetchedBudget == clause.size || log_return_false("[ERROR] (%i,%i) Expected %i freed lits, got %i\n",
            _clause_length, _common_lbd_or_zero, clause.size, fetchedBudget));

        auto lock = _mtx.getLock();
        pushClauseToBack(clause); // absorbs budget, increases # stored clauses
        return true;
    }

    int insert(BufferReader& inputBuffer, ClauseSlot* maxNeighbor) {

        // fast-forward buffer reader to the first applicable slot
        auto& clause = *inputBuffer.getCurrentClausePointer();
        while (clause.begin != nullptr && !fitsThisSlot(clause)) {
            inputBuffer.getNextIncomingClause(); // modifies "clause"
        }

        // There might be multiple slots in the buffer which are applicable for this slot.
        // Iterate over all of them.
        int budget = 0;
        int nbInserted = 0;
        int nbDiscarded = 0;
        while (clause.begin != nullptr && fitsThisSlot(clause)) {

            // Fetch number of clauses in the buffer slot, try and fetch budget accordingly
            int nbInputClauses = inputBuffer.getNumRemainingClausesInBucket()+1;
            budget += tryFetchBudget(nbInputClauses, maxNeighbor, true);

            // Insert clauses one by one
            auto lock = _mtx.getLock();
            while (nbInputClauses > 0) {
                assert(fitsThisSlot(clause));
                if (budget >= _clause_length) {
                    pushClauseToBack(clause);
                    budget -= _clause_length;
                    nbInserted++;
                } else {
                    nbDiscarded++;
                }
                nbInputClauses--;
                inputBuffer.getNextIncomingClause(); // modifies "clause"
            }
        }

        // return excess budget
        if (budget > 0) storeBudget(budget);

        if (_hist_discarded_cls) _hist_discarded_cls->increase(_clause_length, nbDiscarded);
        return nbInserted;
    }

    bool pop(Mallob::Clause& clause) {
        return pop(clause, false);
    }

    bool popWeak(Mallob::Clause& clause) {
        return pop(clause, true);
    }

    enum FlushMode {FLUSH_FITTING, FLUSH_OR_DISCARD_ALL};
    void flushAndShrink(BufferBuilder& buf, std::function<void(int*)> clauseDataConverter,
        FlushMode flushMode = FLUSH_FITTING) {

        // Acquire lock
        auto lock = _mtx.getLock();

        //std::string report;
        //for (size_t i = 0; i < _data_size; i++) report += std::to_string(_data[i]) + " ";
        //LOG(V2_INFO, "SLOT (%i,%i) BEFORE FLUSH: %s\n", _clause_length, _common_lbd_or_zero, report.c_str());

        // Fetch clauses as long as possible
        std::vector<Mallob::Clause> flushedClauses;
        int nbRemainingLits = buf.getMaxRemainingLits();
        nbRemainingLits -= (nbRemainingLits % _clause_length);
        const int nbStoredLits = getNbStoredLiterals();
        int nbFreedLits = tryFreeStoredLiterals(nbRemainingLits, true);
        assert(nbFreedLits >= 0);
        assert(nbFreedLits % _clause_length == 0);
        //LOG(V2_INFO, "  %i/%i lits freed\n", nbFreedLits, nbStoredLits);
        storeBudget(nbFreedLits); // return budget freed from flushed clauses to global budget
        int dataIdx = _data_size - _effective_clause_length;
        while (nbFreedLits >= _clause_length) {
            // Clause freed - read, append to buffer, pop
            readClause(dataIdx, _tmp_clause);
            clauseDataConverter(_tmp_clause.begin);
            flushedClauses.push_back(_tmp_clause);
            nbFreedLits -= _clause_length;
            dataIdx -= _effective_clause_length;
        }
        assert(nbFreedLits == 0 || log_return_false("[ERROR] Slot (%i,%i): %i stray free lits left\n",
            _clause_length, _common_lbd_or_zero, nbFreedLits));
        std::sort(flushedClauses.begin(), flushedClauses.end());
        for (auto& cls : flushedClauses) {
            popFreedClauseAtBack();
            bool success = buf.append(cls);
            assert(success);
        }
        
        if (flushMode == FLUSH_OR_DISCARD_ALL) {
            // Mark any remaining clauses to be discarded
            tryFreeStoredLiterals(_clause_length * _nb_stored_clauses.load(std::memory_order_relaxed),
                true);
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
        int nbDesiredCls = (int) std::ceil(nbDesiredLits / (double)_clause_length);
        while (nbClauses > 0 &&
            !_nb_stored_clauses.compare_exchange_strong(nbClauses, std::max(0, nbClauses-nbDesiredCls), std::memory_order_relaxed)) {}
        //LOG(V2_INFO, "(%i,%i) UPDATE_STOREDCLS %i ~> %i\n",
        //    _clause_length, _common_lbd_or_zero, nbClauses, std::max(0, nbClauses-nbDesiredCls));
        
        int nbFreedLits = std::min(nbClauses, nbDesiredCls)*_clause_length;
        if (!freePartially && nbFreedLits < nbDesiredLits) {
            if (nbFreedLits > 0) storeBudget(nbFreedLits);
            return 0;
        }
        if (nbFreedLits > nbDesiredLits) {
            storeBudget(nbFreedLits-nbDesiredLits);
            nbFreedLits = nbDesiredLits;
        }
        return nbFreedLits;
    }

    int getNbStoredLiterals() const {
        return _nb_stored_clauses.load(std::memory_order_relaxed) * _clause_length;
    }

    int getClauseLength() const {
        return _clause_length;
    }

    bool fitsThisSlot(const Mallob::Clause& clause) {
        return clause.size == _clause_length && 
            (hasIndividualLbds() || clause.lbd == _common_lbd_or_zero);
    }

private:

    void shrink() {
        if (_data.capacity() > 2*_data_size) {
            _data.resize(_data_size);
            _data.shrink_to_fit();
        }
    }

    // While the used budget count and the actual data_size are incoherent,
    // remove a clause from the back of the data and reduce data_size.
    void discardFreedClauses() {
        while (_data_size / _effective_clause_length > _nb_stored_clauses.load(std::memory_order_relaxed)) {
            readClauseAtBack(_tmp_clause);
            if (_hist_discarded_cls) {
                _hist_discarded_cls->increment(_clause_length);
                _cb_discard_cls(_tmp_clause);
            }
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
        readClauseAtBackCopying(clause);
        popFreedClauseAtBack();

        // Return budget which the clause took
        storeBudget(_clause_length);

        // Now that you are owning the lock, also discard clauses
        // which were marked to be discarded.
        discardFreedClauses();

        _mtx.unlock();
        return true;
    }

    void pushClauseToBack(const Mallob::Clause& clause) {
        int nbStoredClsBefore = _nb_stored_clauses.fetch_add(1);
        //LOG(V2_INFO, "(%i,%i) UPDATE_STOREDCLS %i ~> %i\n",
        //    _clause_length, _common_lbd_or_zero, nbStoredClsBefore, nbStoredClsBefore+1);
        if (_data.size() < _data_size + _effective_clause_length) {
            _data.resize(std::max(
                (int) (_data_size+_effective_clause_length),
                (int) std::ceil(1.5 * _data.capacity())
            ));
        }
        auto pos = _data_size;
        _data_size += _effective_clause_length;

        if (hasIndividualLbds()) _data[pos++] = clause.lbd;
        for (size_t i = 0; i < clause.size; ++i) _data[pos++] = clause.begin[i];
    }

    void readClauseAtBack(Mallob::Clause& clause) {
        return readClause(_data_size - _effective_clause_length, clause);
    }

    void readClauseAtBackCopying(Mallob::Clause& clause) {
        return readClauseCopying(_data_size - _effective_clause_length, clause);
    }

    void readClause(int dataIdx, Mallob::Clause& clause) {
        clause.size = _clause_length;
        int* clsData = _data.data() + dataIdx;
        if (hasIndividualLbds()) clause.lbd = *(clsData++);
        else clause.lbd = _common_lbd_or_zero;
        clause.begin = clsData;
    }

    void readClauseCopying(int dataIdx, Mallob::Clause& clause) {
        clause.size = _clause_length;
        int* clsData = _data.data() + dataIdx;
        if (hasIndividualLbds()) clause.lbd = *(clsData++);
        else clause.lbd = _common_lbd_or_zero;
        assert(clause.begin != nullptr);
        memcpy(clause.begin, clsData, sizeof(int) * _clause_length);
    }

    void popFreedClauseAtBack() {
        assert(_data_size >= _effective_clause_length);
        _data_size -= _effective_clause_length;
    }

    int tryFetchBudget(int nbClauses, ClauseSlot* maxNeighbor, bool fetchPartially) {

        int nbDesiredLits = nbClauses*_clause_length;

        // Fetch budget from global budget
        int budget = fetchBudget(nbDesiredLits);
        //LOG(V2_INFO, "(%i,%i) fetched %i/%i\n", _clause_length, _common_lbd_or_zero, budget, nbDesiredLits);
        // Insufficient?
        if (budget < nbDesiredLits) {
            // Try fetch budget from other slots
            ClauseSlot* neighbor = maxNeighbor;
            while (neighbor != this && budget < nbDesiredLits) {
                assert(_clause_length < neighbor->_clause_length);
                auto stolen = neighbor->tryFreeStoredLiterals(nbDesiredLits-budget, true);
                budget += stolen;
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
        return nbDesiredLits;
    }

    int fetchBudget(int numDesired) {
        int globalBudget = _global_budget.load(std::memory_order_relaxed);
        while (globalBudget > 0) {
            int amountToFetch = std::min(globalBudget, numDesired);
            if (_global_budget.compare_exchange_strong(globalBudget, globalBudget-amountToFetch, 
                std::memory_order_relaxed)) {
                // success
                //LOG(V2_INFO, "FETCH_BUDGET %i\n", amountToFetch);
                return amountToFetch;
            }
        }
        //LOG(V2_INFO, "FETCH_BUDGET 0\n");
        return 0;
    }

    void storeBudget(int amount) {
        //LOG(V2_INFO, "STORE_BUDGET %i\n", amount);
        _global_budget.fetch_add(amount, std::memory_order_relaxed);
    }

    bool hasIndividualLbds() const {
        return _common_lbd_or_zero==0;
    }
};
