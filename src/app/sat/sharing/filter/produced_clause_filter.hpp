
#pragma once

#include <array>

#include "util/tsl/robin_map.h"
#include "../../data/produced_clause.hpp"
#include "../../data/produced_clause_candidate.hpp"
#include "util/sys/threading.hpp"

// Packed struct to get in all meta data within a 32 bit integer.
struct ClauseInfo {
 
    // Best LBD so far this clause was PRODUCED (+inserted into buffer) with
    uint8_t minProducedLbd:5;
    // Best LBD so far this clause was SHARED to all solvers with
    uint8_t minSharedLbd:5;
    // Bitset of which local solver(s) exported the clause 
    uint8_t producers:6;
    // Epoch of last modification (production, or sharing:=true)
    uint16_t lastSharedEpoch:16;

    ClauseInfo() {
        minProducedLbd = 0;
        minSharedLbd = 0;
        producers = 0;
        lastSharedEpoch = 0;
    }
    ClauseInfo(const ProducedClauseCandidate& c) {
        minProducedLbd = c.lbd;
        minSharedLbd = 0;
        producers = 1 << c.producerId;
        lastSharedEpoch = 0;
    }
};

// Exact data structure which remembers clauses which were successfully exported by a solver.
// For each incoming clause, the structure can then be used to decide (a) if the clause should 
// be discarded ("filtered") because it was shared before (or too recently) and (b) which
// subset of solvers should receive the clauses (because they did not export it themselves).
// The structure takes space linear in the number of clauses successfully added to the
// AdaptiveClauseDatabase instance which is used for tryRegisterAndInsert. 
class ProducedClauseFilter {

template <typename T>
using ProducedMap = tsl::robin_map<T, ClauseInfo, ProducedClauseHasher<T>, ProducedClauseEqualsCommutative<T>>;

private:
    ProducedMap<ProducedUnitClause> _map_units;
    ProducedMap<ProducedBinaryClause> _map_binaries;
    ProducedMap<ProducedLargeClause> _map_large_clauses;

    Mutex _map_mutex;

    const int _epoch_horizon;
    const bool _reshare_improved_lbd;

    ClauseInfo _empty_clause_info;

public:
    ProducedClauseFilter(int epochHorizon, bool reshareImprovedLbd) : 
        _epoch_horizon(epochHorizon), _reshare_improved_lbd(reshareImprovedLbd) {}

    enum ExportResult {ADMITTED, FILTERED, DROPPED};
    ExportResult tryRegisterAndInsert(ProducedClauseCandidate&& c, AdaptiveClauseDatabase& cdb) {
        
        if (c.size == 1) {
            ProducedUnitClause pc;
            pc.literal = *c.begin;
            return tryRegisterAndInsert(pc, c, _map_units, cdb);

        } else if (c.size == 2) {
            ProducedBinaryClause pc;
            pc.literals[0] = std::min(c.begin[0], c.begin[1]);
            pc.literals[1] = std::max(c.begin[0], c.begin[1]);
            return tryRegisterAndInsert(pc, c, _map_binaries, cdb);

        } else {
            ProducedLargeClause pc;
            pc.size = c.size;
            pc.data = c.releaseData();
            return tryRegisterAndInsert(pc, c, _map_large_clauses, cdb);
        }
    }

    template<typename T>
    ExportResult tryRegisterAndInsert(T& pc, ProducedClauseCandidate& c, ProducedMap<T>& map, AdaptiveClauseDatabase& cdb) {
        
        // Try to find clause
        auto it = map.find(pc);
                
        // If clause is contained:
        bool contained = it != map.end();
        if (contained) {
            int oldLbd = it.value().minProducedLbd;
            // No improvement in LBD value? Filter clause.
            if (oldLbd > 0 && c.lbd >= oldLbd) {
                updateClauseInfo(c, it.value(), /*updateLbd=*/false);
                return FILTERED;
            }
            // Clause can be accepted (again) due to improved LBD score
        }

        // Try to insert to sharing database
        if (!cdb.addClause(prod_cls::data(pc), c.size, c.lbd, /*sortLargeClause=*/true)) {
            // No space left in database: update meta data, drop clause
            // (Do not update LBD value because the clause was not exported)
            if (contained) updateClauseInfo(c, it.value(), /*updateLbd=*/false);
            return DROPPED;
        }

        // Inserted: do register and set epoch to current epoch
        if (contained) updateClauseInfo(c, it.value(), /*updateLbd=*/true);
        else map.insert({std::move(pc), ClauseInfo(c)});
        return ADMITTED;
    }

    uint8_t getProducers(Mallob::Clause& c, int epoch) {

        if (c.size == 1) {
            ProducedUnitClause pc(c);
            return getProducers(pc, _map_units, epoch);

        } else if (c.size == 2) {
            ProducedBinaryClause pc(c);
            return getProducers(pc, _map_binaries, epoch);

        } else {
            ProducedLargeClause pc;
            pc.size = c.size;
            pc.data = c.begin;
            auto info = getProducers(pc, _map_large_clauses, epoch);
            pc.data = nullptr;
            return info;
        }
    }

    bool admitSharing(Mallob::Clause& c, int epoch) {

        if (c.size == 1) {
            ProducedUnitClause pc(c);
            return admitSharing(pc, _map_units, c.lbd, epoch);

        } else if (c.size == 2) {
            ProducedBinaryClause pc(c);
            return admitSharing(pc, _map_binaries, c.lbd, epoch);

        } else {
            ProducedLargeClause pc;
            pc.data = c.begin;
            pc.size = c.size;
            bool admitted = admitSharing(pc, _map_large_clauses, c.lbd, epoch);
            pc.data = nullptr; // avoid freeing of clause data reference
            return admitted;
        }
    }

    inline bool tryAcquireLock() {return _map_mutex.tryLock();}
    inline void acquireLock() {_map_mutex.lock();}
    inline void releaseLock() {_map_mutex.unlock();}

private:
    void updateClauseInfo(const ProducedClauseCandidate& c, ClauseInfo& info, bool updateLbd) {
        assert(c.lbd > 0);
        if (updateLbd) {
            if (info.minProducedLbd == 0 || info.minProducedLbd > c.lbd) {
                // Improved (or first) LBD
                info.minProducedLbd = c.lbd;
            }
        }
        // Add producing solver as a producer
        info.producers |= (1 << c.producerId);
    }

    template <typename T>
    inline bool admitSharing(const T& pc, ProducedMap<T>& map, int lbd, int epoch) {
        
        auto it = map.find(pc);
        if (it == map.end()) return true; // No entry? -> Admit trivially
        
        // There is a present entry for this clause
        ClauseInfo& info = it.value();
        if (info.minSharedLbd > 0) {
            // Clause was shared before
            if (epoch - info.lastSharedEpoch <= _epoch_horizon) {
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
        return true;
    }

    template <typename T>
    inline uint8_t getProducers(const T& pc, ProducedMap<T>& map, int epoch) {
        auto it = map.find(pc);
        if (it == map.end()) return 0;
        ClauseInfo& info = it.value();
        return info.producers;
    }
};
