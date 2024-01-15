
#pragma once

#include <cstdint>
#include <cstdlib>
#include "app/sat/data/clause.hpp"
#include "app/sat/proof/lrat_line.hpp"
#include "util/hmac_sha256/hmac_sha256.hpp"
#include "util/hmac_sha256/sha256.hpp"
#include "util/logger.hpp"
#include "util/tsl/robin_map.h"
#include "util/tsl/robin_set.h"

class LratChecker {

private:
    struct CompactClause {
        int* data {nullptr};
        int nbLits = 0;
        CompactClause() {}
        CompactClause(const int* data, int nbLits) {
            this->data = (int*) malloc(nbLits * sizeof(int));
            this->nbLits = nbLits;
            for (int i = 0; i < nbLits; i++) this->data[i] = data[i];
        }
        CompactClause(CompactClause&& moved) : data(moved.data), nbLits(moved.nbLits) {
            moved.data = nullptr;
            moved.nbLits = 0;
        }
        CompactClause& operator=(CompactClause&& moved) {
            data = moved.data;
            nbLits = moved.nbLits;
            moved.data = nullptr;
            moved.nbLits = 0;
            return *this;
        }
        ~CompactClause() {
            if (!data) return;
            free(data);
        }
    };
    tsl::robin_map<LratClauseId, CompactClause> _clauses;
    std::vector<int8_t> _var_values;
    std::string _errmsg;
    bool _unsat_proven {false};

    LratClauseId _id_to_add {1};
    std::vector<int> _clause_to_add;
    bool _done_loading {false};

    Sha256Builder _sig_builder;

public:
    LratChecker(int nbVars) : _var_values(nbVars+1, 0) {}

    bool loadLiteral(int lit) {
        if (lit == 0) {
            if (!addAxiomaticClause(_id_to_add, _clause_to_add.data(), _clause_to_add.size())) {
                abort();
                return false;
            }
            _id_to_add++;
            _clause_to_add.push_back(0);
            _sig_builder.update((uint8_t*) _clause_to_add.data(), _clause_to_add.size()*sizeof(int));
            _clause_to_add.clear();
            return true;
        }
        _clause_to_add.push_back(lit);
        return true;
    }
    bool endLoading(std::vector<uint8_t>* outSigOrNull) {
        if (!_clause_to_add.empty()) {
            _errmsg = std::to_string(_clause_to_add.size()) + " literals left in unterminated clause";
            abort();
            return false;
        }
        if (outSigOrNull) *outSigOrNull = _sig_builder.get();
        _done_loading = true;
        return true;
    }

    bool loadOriginalClauses(const int* data, size_t size) {
        LratClauseId id = 1;
        int start = 0, end;
        for (size_t i = 0; i < size; i++) {
            int lit = data[i];
            if (lit == 0) {
                end = i;
                if (!addAxiomaticClause(id, data+start, end-start)) {
                    return false;
                }
                id++;
                start = i+1;
            }
        }
        _done_loading = true;
        return true;
    }

    bool addAxiomaticClause(LratClauseId id, const std::vector<int>& lits) {
        return addAxiomaticClause(id, lits.data(), lits.size());
    }
    bool addClause(LratClauseId id, const std::vector<int>& lits, const std::vector<LratClauseId>& hints) {
        return addClause(id, lits.data(), lits.size(), hints.data(), hints.size());
    }
    bool deleteClause(const std::vector<LratClauseId>& ids) {
        return deleteClause(ids.data(), ids.size());
    }

    bool addAxiomaticClause(LratClauseId id, const int* lits, int nbLits) {
        //LOG(V2_INFO, "IMPORT %lu\n", id);
        auto [it, ok] = _clauses.insert({id, CompactClause{lits, nbLits}});
        if (!ok) _errmsg = "Unsuccessful insertion of clause of ID " + std::to_string(id);
        if (nbLits == 0) _unsat_proven = true; // added top-level empty clause!
        return ok;
    }
    bool addClause(LratClauseId id, const int* lits, int nbLits, const LratClauseId* hints, int nbHints) {
        //LOG(V2_INFO, "PRODUCE %lu\n", id);
        if (!checkClause(lits, nbLits, hints, nbHints)) return false;
        return addAxiomaticClause(id, lits, nbLits);
    }
    bool deleteClause(const LratClauseId* ids, int nbIds) {
        for (int i = 0; i < nbIds; i++) {
            auto id = ids[i];
            //LOG(V2_INFO, "DELETE %lu\n", id);
            auto it = _clauses.find(id);
            if (it == _clauses.end()) {
                // ERROR
                _errmsg = "Clause deletion: ID " + std::to_string(id) + " not found";
                return false;
            }
            _clauses.erase(it);
        }
        return true;
    }
    const std::string& getErrorMessage() const {
        return _errmsg;
    }

    bool validateUnsat() {
        if (!_done_loading) {
            _errmsg = "Loading the original formula was not concluded";
            return false;
        }
        if (!_unsat_proven) {
            _errmsg = "Did not derive or import empty clause";
            return false;
        }
        return true;
    }

private:
    bool checkClause(const int* lits, int nbLits, const LratClauseId* hints, int nbHints) {

        // Keep track of asserted unit clauses in a stack
        std::vector<int> setUnits;
        setUnits.reserve(nbLits + nbHints);
        // Assume the negation of each literal in the new clause
        for (int i = 0; i < nbLits; i++) {
            int var = abs(lits[i]);
            _var_values[var] = lits[i]>0 ? -1 : 1; // negated
            setUnits.push_back(var); // remember to reset later
        }

        // Traverse the provided hints to derive a conflict, i.e., the empty clause
        bool ok = true;
        for (int i = 0; i < nbHints; i++) {

            // Find the clause for this hint
            auto hintId = hints[i];
            auto hintClsIt = _clauses.find(hintId);
            if (hintClsIt == _clauses.end()) {
                // ERROR - hint not found
                _errmsg = "Clause addition: ID " + std::to_string(hintId) + " not found";
                ok = false; break;
            }

            // Interpret hint clause (should derive a new unit clause)
            auto& hintCls = hintClsIt->second;
            int newUnit = 0;
            for (int litIdx = 0; litIdx < hintCls.nbLits; litIdx++) {
                int lit = hintCls.data[litIdx];
                int var = abs(lit);
                if (_var_values[var] == 0) {
                    // Literal is unassigned
                    if (newUnit != 0) {
                        // ERROR - multiple unassigned literals in hint clause!
                        _errmsg = "Multiple literals unassigned: " + std::to_string(newUnit) + " " + std::to_string(lit);
                        ok = false; break;
                    }
                    newUnit = lit;
                    continue;
                }
                // Literal is fixed
                bool sign = _var_values[var]>0;
                if (sign == (lit>0)) {
                    // ERROR - clause is satisfied, so it does not belong to hints
                    _errmsg = "Clause hint of ID " + std::to_string(hintId) + " is satisfied";
                    ok = false; break;
                }
                // All OK - literal is false, thus (virtually) removed from the clause
            }
            if (!ok) break;
            if (newUnit == 0) {
                // No unassigned literal in the clause && clause not satisfied
                // -> Empty clause derived.
                if (i+1 < nbHints) {
                    // ERROR - not at the final hint yet!
                    _errmsg = "Empty clause produced at hint " + std::to_string(i+1) + "/" + std::to_string(nbHints);
                    ok = false; break;
                }
                // Final hint produced empty clause - everything OK!
                for (int var : setUnits) _var_values[var] = 0; // reset variable values
                return true;
            }
            // Insert the new derived unit clause
            int var = abs(newUnit);
            _var_values[var] = newUnit>0 ? 1 : -1;
            setUnits.push_back(var); // remember to reset later
        }

        // ERROR - something went wrong
        for (int var : setUnits) _var_values[var] = 0; // reset variable values
        return false;
    }

};
