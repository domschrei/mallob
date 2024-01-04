
#pragma once

#include "app/sat/proof/lrat_line.hpp"
#include "app/sat/proof/serialized_lrat_line.hpp"
#include "util/hashing.hpp"
#include "util/logger.hpp"
#include "util/tsl/robin_map.h"
#include <string>

class LratCompactifier {

private:
    tsl::robin_map<LratClauseId, LratClauseId> _map;
    LratClauseId _last_id {0};
    int _nb_original_clauses {0};
    size_t _nb_mapped {0};

    bool _deduplicate;
    // std::basic_string<int> can be more space efficient
    // for short clauses (i.e., no heap allocation).
    typedef std::basic_string<int> BasicClauseContainer;
    struct BasicClauseContainerHasher {
        size_t operator()(const BasicClauseContainer& cc) const {
            size_t h = 7;
            hash_combine(h, cc.size());
            for (int i = 0; i < cc.size(); i++) {
                hash_combine(h, cc[i]);
            }
            return h;
        }
    };
    tsl::robin_map<BasicClauseContainer, LratClauseId, BasicClauseContainerHasher> _clause_to_id;
    tsl::robin_map<LratClauseId, std::pair<BasicClauseContainer, int>> _id_to_clause_and_nb_aliases;

public:
    LratCompactifier(int nbOrigClauses, bool deduplicate)
        : _nb_original_clauses(nbOrigClauses), _deduplicate(deduplicate) {
        
        _nb_mapped = _nb_original_clauses + 1;
    }

    bool handleClauseAddition(SerializedLratLine& line) {
        if (_nb_original_clauses == 0) return true; // empty instance: do nothing

        // original ID
        auto id = line.getId();
        if (id <= _last_id) {
            LOG(V0_CRIT, "[ERROR] Proof to compactify is not sorted (read ID %lu after %lu\n", id, _last_id);
            abort();
        }
        _last_id = id;

        // check if this clause occurred before (with another ID)
        if (_deduplicate) {
            if (checkDuplicate(line)) return false;
        } else {
            // new clause - map to new ("next") ID
            line.getId() = _nb_mapped++;
            _map[id] = line.getId();
        }

        // map hints (which must already exist)
        auto [hints, nbHints] = line.getHints();
        for (size_t i = 0; i < nbHints; i++) {
            LratClauseId& hint = hints[i];
            if (hint > _nb_original_clauses) {
                hint = _map.at(hint);
            }
        }
        return true;
    }

    bool handleClauseDeletion(int nbHints, LratClauseId* hints) {
        if (_nb_original_clauses == 0) return true; // accept

        // walk through all hints, accept the line iff
        // at least one deletion is valid and should be output
        bool hasAcceptedDeletion = !_deduplicate;
        for (size_t i = 0; i < nbHints; i++) {
            LratClauseId& hint = hints[i];
            if (hint <= _nb_original_clauses) continue;
            auto it = _map.find(hint);
            assert(it != _map.end());
            hint = it->second;
            if (!_deduplicate) {
                // No deduplication: Just erase the ID
                _map.erase(it);
                continue;
            }
            // Need to update and check the clause's ref count.
            auto aliasIt = _id_to_clause_and_nb_aliases.find(hint);
            assert(aliasIt != _id_to_clause_and_nb_aliases.end());
            auto& [clause, nbAliases] = aliasIt.value();
            nbAliases--;
            if (nbAliases == 0) {
                // No aliases (any more):
                // Erase this clause completely.
                LOG(V6_DEBGV, "COMP DEL %lu\n", hint);
                _map.erase(it);
                _clause_to_id.erase(clause);
                _id_to_clause_and_nb_aliases.erase(aliasIt);
                hasAcceptedDeletion = true;
            } else {
                // Still some alias(es) of this clause left:
                // do NOT delete.
                hint = 0; // remove hint from this deletion
                LOG(V6_DEBGV, "COMP RED %lu (%i)\n", hint, nbAliases);
            }
        }
        return hasAcceptedDeletion;
    }

    bool checkDuplicate(SerializedLratLine& line) {
        if (!_deduplicate) return false;
        auto [lits, size] = line.getLiterals();

        // create clause struct with sorted literals (to efficiently check equivalence)
        auto clause = BasicClauseContainer(lits, lits+size);
        std::sort(clause.data(), clause.data()+clause.size());

        // do we already know this clause?
        auto it = _clause_to_id.find(clause);
        if (it != _clause_to_id.end()) {
            // -- yes : clause occurred before
            LratClauseId mappedId = it.value();
            // Map this line's ID to the earlier ID of this clause
            _map[line.getId()] = mappedId;
            // increment reference count
            auto itRef = _id_to_clause_and_nb_aliases.find(mappedId);
            assert(itRef != _id_to_clause_and_nb_aliases.end());
            auto& pair = itRef.value();
            pair.second += 1;
            LOG(V6_DEBGV, "COMP ALIAS %lu => %lu\n", line.getId(), mappedId);
            return true; // duplicate!
        }
        // clause did not occur before

        // map to new ("next") ID
        auto id = line.getId();
        line.getId() = _nb_mapped++;
        _map[id] = line.getId();

        // insert to clause<->ID maps for subsequent duplicates
        _clause_to_id.insert(it, {
            clause,
            line.getId()
        });
        _id_to_clause_and_nb_aliases.insert(std::pair(
            line.getId(),
            std::pair(std::move(clause), 1)
        ));

        LOG(V6_DEBGV, "COMP NEW %lu\n", line.getId());
        return false; // no duplicate
    }
};
