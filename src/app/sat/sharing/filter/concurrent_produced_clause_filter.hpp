
#pragma once

#include <array>
#include <atomic>
#include <variant>
#include <shared_mutex>

#include "app/sat/data/clause.hpp"
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
// The structure takes space linear in the number of clauses successfully added to the
// AdaptiveClauseDatabase instance which is used for tryRegisterAndInsert. 
class ConcurrentProducedClauseFilter {

using ProducedMap = libcuckoo::cuckoohash_map<AnyProducedClause, ClauseInfo, AnyProducedClauseHasher, AnyProducedClauseEquals>;

private:
    AdaptiveClauseDatabase& _cdb;
    std::shared_mutex _mtx_map;
    ProducedMap _map;

    const int _epoch_horizon;
    const bool _reshare_improved_lbd;

    Mutex _mtx_deletion_list;
    std::list<Mallob::Clause> _deletion_list;

    std::atomic_int _epoch {0};
    BackgroundWorker _gc;


public:
    ConcurrentProducedClauseFilter(AdaptiveClauseDatabase& cdb, int epochHorizon, bool reshareImprovedLbd) : 
        _cdb(cdb), _epoch_horizon(epochHorizon), _reshare_improved_lbd(reshareImprovedLbd) {

        _cdb.setClauseDeletionCallback([&](Mallob::Clause& clause) {
            auto lock = _mtx_deletion_list.getLock();
            _deletion_list.push_back(std::move(clause));
        });

        // Garbage collector for old clauses in the map 
        if (_epoch_horizon < 0) return;
        _gc.run([&]() {

            int lastCheckEpoch = 0;

            while (_gc.continueRunning()) {

                usleep(1000 * 1000 * 1); // 1 second

                int epoch = _epoch.load(std::memory_order_relaxed);
                if (epoch - lastCheckEpoch < _epoch_horizon) continue;
                lastCheckEpoch = epoch;

                // Signal that the sweep operation is ongoing
                // to inserting threads calling tryGetSharedLock()
                _mtx_map.lock();

                // Remove all old clauses
                int nbRemoved = 0;
                auto lockedMap = _map.lock_table();
                for (auto it = lockedMap.begin(); it != lockedMap.end();) {
                    auto& [apc, info] = *it;
                    if (epoch - info.lastSharedEpoch > _epoch_horizon) {
                        it = lockedMap.erase(it);
                        nbRemoved++;
                    } else ++it;
                }
                //LOG(V2_INFO, "SWEEPED OVER FILTER horizon=%i epoch=%i removed=%i\n", 
                //    _epoch_horizon, epoch, nbRemoved);
                lockedMap.unlock();

                // Allow inserting threads to successfully tryGetSharedLock() again
                _mtx_map.unlock();
            }
        });
    }

    enum ExportResult {ADMITTED, FILTERED, DROPPED};
    ExportResult tryRegisterAndInsert(ProducedClauseCandidate&& c) {

        ClauseInfo defaultInfo(c);
        defaultInfo.lastSharedEpoch = _epoch.load(std::memory_order_relaxed);
        AnyProducedClause apc = getAnyProducedClause(c);
        int* data = getLiteralData(apc);

        ExportResult result;
        _map.uprase_fn(apc, [&](ClauseInfo& info, libcuckoo::UpsertContext ctx) {

            bool contained = ctx == libcuckoo::UpsertContext::ALREADY_EXISTED;
            if (contained) {
                // entry existed before: check if this clause should be filtered
                int oldLbd = info.minProducedLbd;
                // No resharing upon improved LBD, or LBD not improved?
                // => Filter clause.
                if (!_reshare_improved_lbd || (oldLbd > 0 && c.lbd >= oldLbd)) {
                    // add new producer, return.
                    updateClauseInfo(c, info, /*updateLbd=*/false);
                    result = FILTERED;
                    return false;
                }
                // Clause can be accepted (again) due to improved LBD score
            }

            // Try to insert to sharing database
            if (_cdb.addClause(data, c.size, c.lbd)) {
                // Success!
                result = ADMITTED;
                if (!contained) return false; // nothing to do, info was newly constructed 
                updateClauseInfo(c, info, /*updateLbd=*/true);
            } else {
                // No space left in database: update meta data, drop clause
                result = DROPPED;
                if (!contained) return true; // delete the newly constructed entry again
                // (Do not update LBD value because the clause was not exported)
                updateClauseInfo(c, info, /*updateLbd=*/false);
            }

            return false; // do not delete
        }, defaultInfo);

        //LOG(V2_INFO, "FILTER_SIZE %lu/%lu\n", _map.size(), _map.capacity());

        if (result == ADMITTED) {
            // Remove up to two clauses which are marked for deletion from the filter
            Mallob::Clause c1, c2;
            {
                auto lock = _mtx_deletion_list.getLock();
                if (!_deletion_list.empty()) {
                    c1 = std::move(_deletion_list.front());
                    _deletion_list.pop_front();
                }
                if (!_deletion_list.empty()) {
                    c2 = std::move(_deletion_list.front());
                    _deletion_list.pop_front();
                }
            }
            if (c1.begin != nullptr) erase(c1);
            if (c2.begin != nullptr) erase(c2);
        }

        return result;
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

    void erase(ProducedClauseCandidate& c) {
        _map.erase(getAnyProducedClause(c));
    }
    void erase(Mallob::Clause& c) {
        auto apc = getAnyProducedClause(c);
        _map.erase(apc);
        if (apc.index() == 2) std::get<2>(apc).data = nullptr;
    }

    size_t size() const {
        return _map.size();
    }

    bool tryGetSharedLock() {
        return _mtx_map.try_lock_shared();
    }
    void returnSharedLock() {
        _mtx_map.unlock_shared();
    }

private:
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

    void updateClauseInfo(const ProducedClauseCandidate& c, ClauseInfo& info, bool updateLbd) {
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
    }

    inline bool admitSharing(const AnyProducedClause& apc, int lbd, int epoch) {
        
        bool admit = true;
        _map.update_fn(apc, [&](ClauseInfo& info) {

            if (info.minSharedLbd > 0) {
                // Clause was shared before
                if (_epoch_horizon < 0 || epoch - info.lastSharedEpoch <= _epoch_horizon) {
                    // Clause was shared at some recent point in time
                    if (!_reshare_improved_lbd) {
                        // Never reshare recent clauses, even with improved LBD
                        admit = false;
                        return;
                    }
                    if (info.minSharedLbd <= lbd) {
                        // Clause was shared with this LBD or better: filter
                        admit = false; 
                        return;
                    }
                }
            }

            // Admit for sharing, update meta data to reflect sharing
            info.minSharedLbd = lbd;
            info.lastSharedEpoch = epoch;
            admit = true;
        });

        return admit;
    }

    inline cls_producers_bitset getProducers(const AnyProducedClause& apc, int epoch) {
        ClauseInfo returnedInfo;
        bool found = _map.find(apc, returnedInfo);
        if (!found) return 0;
        return returnedInfo.producers;
    }
};
