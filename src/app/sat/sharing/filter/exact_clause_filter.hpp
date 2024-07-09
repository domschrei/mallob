
#pragma once

#include <atomic>
#include <variant>

#include "app/sat/data/clause.hpp"
#include "app/sat/sharing/store/generic_clause_store.hpp"
#include "app/sat/sharing/filter/generic_clause_filter.hpp"
#include "util/logger.hpp"
#include "util/tsl/robin_map.h"
#include "../../data/produced_clause.hpp"
#include "../../data/produced_clause_candidate.hpp"
#include "util/sys/threading.hpp"
#include "produced_clause_filter_commons.hpp"
#include "util/sys/timer.hpp"

typedef std::variant<ProducedUnitClause, ProducedBinaryClause, ProducedLargeClause> AnyProducedClause;

struct AnyProducedClauseHasher {
    std::size_t inline operator()(const AnyProducedClause& anyPC) const {
        switch (anyPC.index()) {
        case 0:
            return Mallob::nonCommutativeHash(prod_cls::data(std::get<0>(anyPC)),
                prod_cls::size(std::get<0>(anyPC)), 1);
        case 1:
            return Mallob::nonCommutativeHash(prod_cls::data(std::get<1>(anyPC)),
                prod_cls::size(std::get<1>(anyPC)), 2);
        case 2:
        default:
            return Mallob::nonCommutativeHash(prod_cls::data(std::get<2>(anyPC)),
                prod_cls::size(std::get<2>(anyPC)), 3);
        }
    }
};
struct AnyProducedClauseEquals {
    bool inline operator()(const AnyProducedClause& a, const AnyProducedClause& b) const {
        static ProducedClauseEquals<ProducedUnitClause> equalsUnit;
        static ProducedClauseEquals<ProducedBinaryClause> equalsBinary;
        static ProducedClauseEquals<ProducedLargeClause> equalsLarge;
        if (a.index() != b.index()) return false;
        switch (a.index()) {
        case 0:
            return equalsUnit(std::get<0>(a), std::get<0>(b));
        case 1:
            return equalsBinary(std::get<1>(a), std::get<1>(b));
        case 2:
        default:
            return equalsLarge(std::get<2>(a), std::get<2>(b));
        }
    }
};

// Exact data structure which remembers clauses which were successfully exported by a solver.
// For each incoming clause, the structure can then be used to decide (a) if the clause should 
// be discarded ("filtered") because it was shared before (or too recently) and (b) which
// subset of solvers should receive the clauses (because they did not export it themselves).
class ExactClauseFilter : public GenericClauseFilter {

using ProducedMap = tsl::robin_map<AnyProducedClause, ClauseInfo, AnyProducedClauseHasher, AnyProducedClauseEquals>;

private:
    const int _epoch_horizon;

    struct Slot {
        Mutex _mtx_map;
        ProducedMap _map;
        Slot(GenericClauseStore& clauseStore, int clauseLength) : _map(32'768) {}
    };
    std::vector<std::unique_ptr<Slot>> _slots;
    
    int _last_gc_epoch {0};

public:
    ExactClauseFilter(GenericClauseStore& clauseStore, int epochHorizon, int maxEffClauseLength) :
        GenericClauseFilter(clauseStore), _epoch_horizon(epochHorizon),
        _slots(maxEffClauseLength) {

        for (size_t i = 0; i < _slots.size(); i++) {
            _slots[i].reset(new Slot(_clause_store, i+1));
        }
    }

    ExportResult tryRegisterAndInsert(ProducedClauseCandidate&& c, GenericClauseStore* storeOrNullptr = nullptr) override {
        Mallob::Clause cls;

        AnyProducedClause apc = getAnyProducedClause(c);
        int* data = getLiteralData(apc);

        ExportResult result;

        auto& slot = getSlot(c.size);
        auto it = slot._map.find(apc);
        bool contained = it != slot._map.end();
        bool filtered = false;

        if (contained) {
            // entry existed before: check if the clause should be filtered.
            auto& info = it->second;
            if (!info.isAdmissibleForInsertion(c.epoch, _epoch_horizon)) {
                // filtered! add new producer, return.
                updateClauseInfo(c, apc, it, false);
                result = FILTERED;
                filtered = true;
            }
        }
        if (!filtered) {
            // Try to insert clause to clause store
            cls.begin = data; cls.size = c.size; cls.lbd = c.lbd;
            auto clauseStore = storeOrNullptr ? storeOrNullptr : &_clause_store;
            if (clauseStore->addClause(cls)) {
                // Success!
                updateClauseInfo(c, apc, it, true); // create if nonexistent
                result = ADMITTED;
            } else {
                // No space left in database: drop clause
                if (contained) updateClauseInfo(c, apc, it, false); // update if existent
                result = DROPPED;
            }
        }

        return result;
    }

    bool collectGarbage(const Logger& logger) override {

        // Garbage collector for old clauses in the map
        if (_epoch_horizon < 0) return false;

        int epoch = _epoch.load(std::memory_order_relaxed);
        if (epoch - _last_gc_epoch < _epoch_horizon) return false;
        _last_gc_epoch = epoch;

        for (size_t i = 0; i < _slots.size(); i++) {
            auto& slot = *_slots.at(i);
            auto time = Timer::elapsedSeconds();

            // Signal that the sweep operation is ongoing
            // to inserting threads calling tryGetSharedLock()
            slot._mtx_map.lock();

            // Remove all old clauses
            size_t nbRemoved = 0;
            size_t mapSize = slot._map.size();
            for (auto it = slot._map.begin(); it != slot._map.end();) {
                auto& [apc, info] = *it;
                if (epoch - info.lastSharedEpoch > _epoch_horizon
                    && epoch - info.lastProducedEpoch > _epoch_horizon) {
                    it = slot._map.erase(it);
                    nbRemoved++;
                } else ++it;
            }

            time = Timer::elapsedSeconds() - time;
            LOGGER(logger, V5_DEBG, "filter-gc clslen=%i epoch=%i removed=%lu/%lu time=%.4f\n",
                i+1-ClauseMetadata::numInts(), epoch, nbRemoved, mapSize, time);

            // Allow inserting threads to successfully tryGetSharedLock() again
            slot._mtx_map.unlock();
        }

        LOGGER(logger, V4_VVER, "pcb size=%ld %s\n", _clause_store.getCurrentlyUsedLiterals(),
                _clause_store.getCurrentlyUsedLiteralsReport().c_str());
        return true;
    }

    cls_producers_bitset confirmSharingAndGetProducers(Mallob::Clause& c, int epoch) override {
        auto apc = getAnyProducedClause(c);
        auto producers = confirmSharingAndGetProducers(apc, c.size, c.lbd, epoch);
        if (apc.index() == 2) std::get<2>(apc).data = nullptr;
        return producers;
    }

    bool admitSharing(Mallob::Clause& c, int epoch) override {
        auto apc = getAnyProducedClause(c);
        bool admitted = admitSharing(apc, c.size, c.lbd, epoch);
        if (apc.index() == 2) std::get<2>(apc).data = nullptr;
        return admitted;
    }

    size_t size(int clauseLength) const override {
        if (clauseLength == 0) {
            size_t totalSize = 0;
            for (auto& slot : _slots) totalSize += slot->_map.size();
            return totalSize;
        }
        return getSlot(clauseLength)._map.size();
    }

    bool tryAcquireLock(int clauseLength) override {
        return getSlot(clauseLength)._mtx_map.tryLock();
    }
    void acquireLock(int clauseLength) override {
        getSlot(clauseLength)._mtx_map.lock();
    }
    void releaseLock(int clauseLength) override {
        getSlot(clauseLength)._mtx_map.unlock();
    }

    void acquireAllLocks() override {
        std::vector<bool> slotLocked(_slots.size(), false);
        int nbLocked = 0;
        // Repeatedly cycle over the slots, acquiring locks where possible,
        // until all locks are held
        while (nbLocked < _slots.size()) {
            for (size_t i = 0; i < _slots.size(); i++) {
                if (slotLocked[i]) continue;
                if (nbLocked+1 == _slots.size()) {
                    // Last slot: acquire lock directly
                    acquireLock(i+1);
                    nbLocked++;
                    slotLocked[i] = true;
                } else if (tryAcquireLock(i+1)) {
                    nbLocked++;
                    slotLocked[i] = true;
                }
            }
        }
    }
    void releaseAllLocks() override {
        for (size_t i = 0; i < _slots.size(); i++) releaseLock(i+1);
    }

    void erase(ProducedClauseCandidate& c) {
        getSlot(c.size)._map.erase(getAnyProducedClause(c));
    }

private:
    Slot& getSlot(int clauseLength) const {
        assert(clauseLength-1 >= 0 && clauseLength-1 < _slots.size()
            || log_return_false("[ERROR] Invalid clause length %i\n", clauseLength));
        return *_slots.at(clauseLength-1);
    }

    void erase(Mallob::Clause& c) {
        auto apc = getAnyProducedClause(c);
        getSlot(c.size)._map.erase(apc);
        if (apc.index() == 2) std::get<2>(apc).data = nullptr;
    }

    AnyProducedClause getAnyProducedClause(ProducedClauseCandidate& c) {
        AnyProducedClause apc;
        if (c.size == 1) {
            ProducedUnitClause pc;
            pc.literal = *c.begin;
            apc = std::move(pc);
        } else if (c.size == 2) {
            ProducedBinaryClause pc;
            pc.literals[0] = std::min(c.begin[0], c.begin[1]);
            pc.literals[1] = std::max(c.begin[0], c.begin[1]);
            apc = std::move(pc);
        } else {
            ProducedLargeClause pc;
            pc.size = c.size;
            pc.data = c.releaseData();
            apc = std::move(pc);
        }
        return apc;
    }
    AnyProducedClause getAnyProducedClause(Mallob::Clause& c) {
        AnyProducedClause apc;
        if (c.size == 1) {
            apc = ProducedUnitClause(c);
        } else if (c.size == 2) {
            apc = ProducedBinaryClause(c);
        } else {
            ProducedLargeClause pc;
            pc.size = c.size;
            pc.data = c.begin;
            apc = std::move(pc);
            pc.data = nullptr;
        }
        return apc;
    }
    int* getLiteralData(AnyProducedClause& apc) {
        switch (apc.index()) {
        case 0:
            return &std::get<0>(apc).literal;
        case 1:
            return std::get<1>(apc).literals;
        case 2:
        default:
            return std::get<2>(apc).data;
        }
    }

    ClauseInfo getDefaultClauseInfo(const ProducedClauseCandidate& c) {
        return ClauseInfo(c);
    }

    void updateClauseInfo(const ProducedClauseCandidate& c, const AnyProducedClause& apc, ProducedMap::iterator& it,
            bool updateProducedEpoch) {

        auto& slot = getSlot(c.size);
        ClauseInfo info = it == slot._map.end() ? getDefaultClauseInfo(c) : it->second;
        assert(c.producerId < MALLOB_MAX_N_APPTHREADS_PER_PROCESS);
        // Update the epoch where it was last produced 
        if (updateProducedEpoch && c.epoch > info.lastProducedEpoch) info.lastProducedEpoch = c.epoch;
        // Add producing solver as a producer
        info.producers |= (1 << c.producerId);
        slot._map.insert_or_assign(it, apc, std::move(info));
    }

    inline bool admitSharing(const AnyProducedClause& apc, int size, int lbd, int epoch) {

        auto& slot = getSlot(size);
        auto it = slot._map.find(apc);
        if (it == slot._map.end()) return true;

        const ClauseInfo& info = it->second;

        if (!info.isAdmissibleForSharing(epoch, _epoch_horizon)) {
            // Clause was shared at some recent point in time: do not reshare
            return false;
        }

        // Admit for sharing
        return true;
    }

    inline cls_producers_bitset confirmSharingAndGetProducers(const AnyProducedClause& apc, int size, int lbd, int epoch) {

        auto& slot = getSlot(size);
        auto it = slot._map.find(apc);
        if (it == slot._map.end()) return 0;
        ClauseInfo info = it->second;
        info.lastSharedEpoch = epoch;
        // return no producers if all registered producers are from a long time ago
        auto producers =
            (_epoch_horizon >= 0 && epoch - info.lastProducedEpoch > _epoch_horizon) ?
            0 : info.producers;
        info.producers = 0; // reset producers in any case
        slot._map.insert_or_assign(it, apc, std::move(info));
        return producers;
    }
};
