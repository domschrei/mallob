
#pragma once

#include <array>

#include "util/tsl/robin_map.h"
#include "app/sat/hordesat/sharing/database/produced_clause.hpp"
#include "produced_clause_candidate.hpp"
#include "util/sys/threading.hpp"

// Packed struct to get in all meta data within a 32 bit integer.
struct ClauseInfo {

    // Was the clause shared before?
    bool shared:1;
    // Best LBD so far this clause was learnt with
    uint8_t lbd:7;
    // Bitset of which local solver(s) exported the clause 
    uint8_t producers:8;
    // Epoch of last modification (production, or sharing:=true)
    uint16_t epoch:16;

    ClauseInfo() {
        shared = false;
        lbd = 0;
        producers = 0;
        epoch = -1;
    }
    ClauseInfo(const ProducedClauseCandidate& c) {
        shared = false;
        lbd = c.lbd;
        producers = 1 << c.producerId;
        epoch = c.epoch;
    }
    bool valid() const {return lbd > 0;}
};

class ProducedClauseFilter {

template <typename T>
using ProducedMap = tsl::robin_map<T, ClauseInfo, ProducedClauseHasher<T>, ProducedClauseEqualsCommutative<T>>;

private:
    ProducedMap<ProducedUnitClause> _map_units;
    ProducedMap<ProducedBinaryClause> _map_binaries;
    ProducedMap<ProducedLargeClause> _map_large_clauses;

    Mutex _map_mutex;

    const int _epoch_horizon;

    ClauseInfo _empty_clause_info;

public:
    ProducedClauseFilter(int epochHorizon) : _epoch_horizon(epochHorizon) {
        assert(!_empty_clause_info.valid());
    }

    enum ExportResult {ADMITTED, FILTERED, DROPPED};
    // Probes whether the provided clause is novel (i.e., not present yet).
    // Does NOT insert the clause. If the clause is not novel, its meta data
    // is updated by the provided instance of the clause, and false is returned. 
    // If the clause is novel, no modifications are made, and true is returned.
    ExportResult tryRegisterAndInsert(ProducedClauseCandidate&& c, AdaptiveClauseDatabase& cdb) {
        
        if (c.size == 1) {
            ProducedUnitClause pc;
            pc.literal = *c.begin;
            auto it = _map_units.find(pc);
            if (it != _map_units.end()) return updateClauseInfo(c, it.value());
            // Can insert
            if (!cdb.addClause(c.begin, c.size, c.lbd)) return DROPPED;
            // Inserted: do register
            _map_units.insert({std::move(pc), ClauseInfo(c)});
            return ADMITTED;

        } else if (c.size == 2) {
            ProducedBinaryClause pc;
            pc.literals[0] = std::min(c.begin[0], c.begin[1]);
            pc.literals[1] = std::max(c.begin[0], c.begin[1]);
            auto it = _map_binaries.find(pc);
            if (it != _map_binaries.end()) return updateClauseInfo(c, it.value());
            // Can insert
            if (!cdb.addClause(c.begin, c.size, c.lbd)) return DROPPED;
            // Inserted: do register
            _map_binaries.insert({std::move(pc), ClauseInfo(c)});
            return ADMITTED;

        } else {
            ProducedLargeClause pc;
            pc.size = c.size;
            pc.data = c.releaseData();
            auto it = _map_large_clauses.find(pc);
            if (it != _map_large_clauses.end()) return updateClauseInfo(c, it.value());
            // Try to insert to buffer (this will sort the underlying data if successful)
            if (!cdb.addClause(pc.data, c.size, c.lbd, /*sortLargeClause=*/true)) return DROPPED;
            // Inserted: do register
            _map_large_clauses.insert({std::move(pc), ClauseInfo(c)});
            return ADMITTED;
        }
    }

    ClauseInfo getInfo(Mallob::Clause& c, int epoch) {

        if (c.size == 1) {
            ProducedUnitClause pc(c);
            return getInfo(pc, _map_units, epoch);

        } else if (c.size == 2) {
            ProducedBinaryClause pc(c);
            return getInfo(pc, _map_binaries, epoch);

        } else {
            ProducedLargeClause pc;
            pc.size = c.size;
            pc.data = c.begin;
            auto info = getInfo(pc, _map_large_clauses, epoch);
            pc.data = nullptr;
            return info;
        }
    }

    bool admitSharing(Mallob::Clause& c, int epoch) {

        if (c.size == 1) {
            ProducedUnitClause pc(c);
            return admitSharing(pc, _map_units, epoch);

        } else if (c.size == 2) {
            ProducedBinaryClause pc(c);
            return admitSharing(pc, _map_binaries, epoch);

        } else {
            ProducedLargeClause pc;
            pc.data = c.begin;
            pc.size = c.size;
            bool admitted = admitSharing(pc, _map_large_clauses, epoch);
            pc.data = nullptr; // avoid freeing of clause data reference
            return admitted;
        }
    }

    inline bool tryAcquireLock() {return _map_mutex.tryLock();}
    inline void acquireLock() {_map_mutex.lock();}
    inline void releaseLock() {_map_mutex.unlock();}

private:
    ExportResult updateClauseInfo(const ProducedClauseCandidate& c, ClauseInfo& info) {
        assert(info.valid());
        info.producers |= (1 << c.producerId);
        if (info.lbd == 0) {
            info.lbd = c.lbd;
        } else {
            info.lbd = std::min(info.lbd, static_cast<uint8_t>(c.lbd));
        }
        assert(c.lbd > 0);
        info.epoch = c.epoch;
        return FILTERED;
    }

    template <typename T>
    inline bool admitSharing(const T& pc, ProducedMap<T>& map, int epoch) {
        auto& info = getInfo(pc, map, epoch);
        if (!info.valid()) return true; // clause is not registered
        if (info.shared) return false; // clause was already shared
        info.shared = true; // mark as shared
        return true;
    }

    template <typename T>
    inline ClauseInfo& getInfo(const T& pc, ProducedMap<T>& map, int epoch) {
        auto it = map.find(pc);
        if (it == map.end()) return _empty_clause_info;
        ClauseInfo& info = it.value();
        if (epoch - info.epoch > _epoch_horizon) {
            // Clause grew too old: erase
            map.erase(it);
            return _empty_clause_info;
        }
        return info;
    }
};
