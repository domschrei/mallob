
#pragma once

#include <array>
#include <atomic>
#include <variant>
#include <shared_mutex>

#include "app/sat/data/clause.hpp"
#include "app/sat/sharing/buffer/priority_clause_buffer.hpp"
#include "util/libcuckoo/cuckoohash_map.hh"
#include "util/libcuckoo/cuckoohash_util.hh"
#include "util/logger.hpp"
#include "util/sys/process.hpp"
#include "util/tsl/robin_map.h"
#include "../../data/produced_clause.hpp"
#include "../../data/produced_clause_candidate.hpp"
#include "util/sys/threading.hpp"
#include "../buffer/adaptive_clause_database.hpp"
#include "util/params.hpp"
#include "produced_clause_filter_commons.hpp"
#include "util/sys/background_worker.hpp"

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
class ConcurrentProducedClauseFilter {

using ProducedMap = tsl::robin_map<AnyProducedClause, ClauseInfo, AnyProducedClauseHasher, AnyProducedClauseEquals>;

private:
    PriorityClauseBuffer& _pcb;
    Mutex _mtx_map;
    ProducedMap _map;

    const int _epoch_horizon;
    const bool _reshare_improved_lbd;

    Mutex _mtx_deletion_list;
    std::atomic_int _deletion_list_size {0};
    std::list<Mallob::Clause> _deletion_list;

    std::atomic_int _epoch {0};
    int _last_gc_epoch {0};

public:
    ConcurrentProducedClauseFilter(PriorityClauseBuffer& pcb, int epochHorizon, bool reshareImprovedLbd) :
        _pcb(pcb), _map(32'768), _epoch_horizon(epochHorizon), _reshare_improved_lbd(reshareImprovedLbd) {

        _pcb.setClauseDeletionCallback([&](Mallob::Clause& clause) {
            auto lock = _mtx_deletion_list.getLock();
            _deletion_list_size.fetch_add(1, std::memory_order_relaxed);
            _deletion_list.push_back(std::move(clause));
        });
    }

    enum ExportResult {ADMITTED, FILTERED, DROPPED};
    ExportResult tryRegisterAndInsert(ProducedClauseCandidate&& c) {

        AnyProducedClause apc = getAnyProducedClause(c);
        int* data = getLiteralData(apc);

        ExportResult result;

        auto it = _map.find(apc);
        bool contained = it != _map.end();
        bool filtered = false;

        if (contained) {
            auto& info = it->second;
            // entry existed before: check if this clause should be filtered
            int oldLbd = info.minProducedLbd;
            // No resharing upon improved LBD, or LBD not improved?
            // => Filter clause.
            if (!_reshare_improved_lbd || (oldLbd > 0 && c.lbd >= oldLbd)) {
                // add new producer, return.
                updateClauseInfo(c, apc, it, /*updateLbd=*/false);
                result = FILTERED;
                filtered = true;
            }
            // Clause can be accepted (again) due to improved LBD score
        }

        if (!filtered) {
            // Try to insert to sharing database
            if (_pcb.addClause(data, c.size, c.lbd)) {
                // Success!
                result = ADMITTED;
                updateClauseInfo(c, apc, it, /*updateLbd=*/true);
            } else {
                // No space left in database: update meta data, drop clause
                result = DROPPED;
                // (Do not update LBD value because the clause was not exported)
                if (contained) updateClauseInfo(c, apc, it, /*updateLbd=*/false);
            }
        }

        if (result == ADMITTED && _deletion_list_size.load(std::memory_order_relaxed) >= 2) {
            // Remove up to two clauses which are marked for deletion from the filter
            Mallob::Clause c1, c2;
            {
                auto lock = _mtx_deletion_list.getLock();
                if (!_deletion_list.empty()) {
                    c1 = std::move(_deletion_list.front());
                    _deletion_list.pop_front();
                    _deletion_list_size.fetch_sub(1, std::memory_order_relaxed);
                }
                if (!_deletion_list.empty()) {
                    c2 = std::move(_deletion_list.front());
                    _deletion_list.pop_front();
                    _deletion_list_size.fetch_sub(1, std::memory_order_relaxed);
                }
            }
            if (c1.begin != nullptr) erase(c1);
            if (c2.begin != nullptr) erase(c2);
        }

        return result;
    }

    void collectGarbage(const Logger& logger, int clauseLength) {

        // Garbage collector for old clauses in the map
        if (_epoch_horizon < 0) return;

        int epoch = _epoch.load(std::memory_order_relaxed);
        if (epoch - _last_gc_epoch < _epoch_horizon) return;
        _last_gc_epoch = epoch;

        auto time = Timer::elapsedSeconds();

        // Signal that the sweep operation is ongoing
        // to inserting threads calling tryGetSharedLock()
        _mtx_map.lock();

        // Remove all old clauses
        size_t nbRemoved = 0;
        size_t mapSize = _map.size();
        for (auto it = _map.begin(); it != _map.end();) {
            auto& [apc, info] = *it;
            if (epoch - info.lastSharedEpoch > _epoch_horizon) {
                it = _map.erase(it);
                nbRemoved++;
            } else ++it;
        }

        time = Timer::elapsedSeconds() - time;
        LOGGER(logger, V4_VVER, "filter-gc clslen=%i epoch=%i removed=%lu/%lu time=%.4f\n",
            clauseLength, epoch, nbRemoved, mapSize, time);

        // Allow inserting threads to successfully tryGetSharedLock() again
        _mtx_map.unlock();
    }

    void updateEpoch(int epoch) {
        _epoch.store(epoch, std::memory_order_relaxed);
    }

    cls_producers_bitset getProducers(Mallob::Clause& c, int epoch) {
        auto apc = getAnyProducedClause(c);
        auto producers = getProducers(apc, epoch);
        if (apc.index() == 2) std::get<2>(apc).data = nullptr;
        return producers;
    }

    bool admitSharing(Mallob::Clause& c, int epoch) {
        auto apc = getAnyProducedClause(c);
        bool admitted = admitSharing(apc, c.lbd, epoch);
        if (apc.index() == 2) std::get<2>(apc).data = nullptr;
        return admitted;
    }

    size_t size() const {
        return _map.size();
    }

    bool tryAcquireLock() {
        return _mtx_map.tryLock();
    }
    void acquireLock() {
        _mtx_map.lock();
    }
    void releaseLock() {
        _mtx_map.unlock();
    }

private:
    void erase(ProducedClauseCandidate& c) {
        _map.erase(getAnyProducedClause(c));
    }
    void erase(Mallob::Clause& c) {
        auto apc = getAnyProducedClause(c);
        _map.erase(apc);
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
        ClauseInfo defaultInfo(c);
        defaultInfo.lastSharedEpoch = _epoch.load(std::memory_order_relaxed);
        return defaultInfo;
    }

    void updateClauseInfo(const ProducedClauseCandidate& c, const AnyProducedClause& apc, ProducedMap::iterator& it, bool updateLbd) {

        ClauseInfo info = it == _map.end() ? getDefaultClauseInfo(c) : it->second;
        assert(c.lbd > 0);
        if (updateLbd) {
            if (info.minProducedLbd == 0 || info.minProducedLbd > c.lbd) {
                // Improved (or first) LBD
                info.minProducedLbd = c.lbd;
            }
        }
        if (info.minSharedLbd == 0) {
            // clause was not shared before: we abuse the field for the epoch where it was last produced
            info.lastSharedEpoch = c.epoch;
        }
        // Add producing solver as a producer
        assert(c.producerId < MALLOB_MAX_N_APPTHREADS_PER_PROCESS);
        info.producers |= (1 << c.producerId);
        _map.insert_or_assign(it, apc, std::move(info));
    }

    inline bool admitSharing(const AnyProducedClause& apc, int lbd, int epoch) {

        auto it = _map.find(apc);
        if (it == _map.end()) return true;

        ClauseInfo info = it->second;

        if (info.minSharedLbd > 0) {
            // Clause was shared before
            if (_epoch_horizon < 0 || epoch - info.lastSharedEpoch <= _epoch_horizon) {
                // Clause was shared at some recent point in time
                if (!_reshare_improved_lbd) {
                    // Never reshare recent clauses, even with improved LBD
                    return false;
                }
                if (info.minSharedLbd <= lbd) {
                    // Clause was shared with this LBD or better: filter
                    return false;
                }
            }
        }

        // Admit for sharing, update meta data to reflect sharing
        info.minSharedLbd = lbd;
        info.lastSharedEpoch = epoch;
        _map.insert_or_assign(it, apc, std::move(info));
        return true;
    }

    inline cls_producers_bitset getProducers(const AnyProducedClause& apc, int epoch) {
        auto it = _map.find(apc);
        if (it == _map.end()) return 0;
        return it->second.producers;
    }
};
