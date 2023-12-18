
#pragma once

#include "app/sat/proof/lrat_line.hpp"
#include "app/sat/proof/serialized_lrat_line.hpp"
#include "util/logger.hpp"
#include "util/tsl/robin_map.h"

class LratCompactifier {

private:
    tsl::robin_map<LratClauseId, LratClauseId> _map;
    LratClauseId _last_id {0};
    int _nb_original_clauses {0};
    size_t _nb_mapped {0};

public:
    LratCompactifier(int nbOrigClauses)
        : _nb_original_clauses(nbOrigClauses) {
        
        _nb_mapped = _nb_original_clauses + 1;
    }

    void handleClauseAddition(SerializedLratLine& line) {
        if (_nb_original_clauses == 0) return;

        // original ID
        auto id = line.getId();
        if (id <= _last_id) {
            LOG(V0_CRIT, "[ERROR] Proof to compactify is not sorted (read ID %lu after %lu\n", id, _last_id);
            abort();
        }
        _last_id = id;

        // map to new ID
        line.getId() = _nb_mapped++;
        _map[id] = line.getId();

        // map hints (which must already exist)
        auto [hints, nbHints] = line.getHints();
        for (size_t i = 0; i < nbHints; i++) {
            LratClauseId& hint = hints[i];
            if (hint > _nb_original_clauses) hint = _map.at(hint);
        }
    }

    void handleClauseDeletion(int nbHints, LratClauseId* hints) {
        if (_nb_original_clauses == 0) return;

        for (size_t i = 0; i < nbHints; i++) {
            LratClauseId& hint = hints[i];
            if (hint > _nb_original_clauses) {
                auto it = _map.find(hint);
                hint = it->second;
                _map.erase(it);
            } 
        }
    }
};
