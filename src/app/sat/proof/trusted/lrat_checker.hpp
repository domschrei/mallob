
#pragma once

#include "siphash/siphash.hpp"
#include "trusted_utils.hpp"
#include "robin_map.h"

class LratChecker {

private:
    // We represent each clause as a raw, owned, zero-terminated pointer to the literals.
    // This minimizes the memory used by "empty" elements in the large hash table.
    struct CompactClause {
        int* data {nullptr};
        CompactClause(const int* data, int nbLits) {
            this->data = (int*) malloc((nbLits+1) * sizeof(int));
            for (int i = 0; i < nbLits; i++) this->data[i] = data[i];
            this->data[nbLits] = 0;
        }
        CompactClause(CompactClause&& moved) : data(moved.data) {
            moved.data = nullptr;
        }
        CompactClause& operator=(CompactClause&& moved) {
            data = moved.data;
            moved.data = nullptr;
            return *this;
        }
        ~CompactClause() {
            if (data) free(data);
        }
    };

    // Simple and fast hash function for clause IDs.
    struct ClauseIdHasher {
        std::size_t operator()(const u64& val) const {
            return (0xcbf29ce484222325UL ^ val) * 0x00000100000001B3UL;
        }
    };

    // The hash table where we keep all clauses and which uses most of our RAM.
    // We still use a power-of-two growth policy since this makes lookups faster.
    tsl::robin_map<u64, CompactClause, ClauseIdHasher> _clauses;

    std::vector<int8_t> _var_values;
    char _errmsg[512] = {0};
    bool _unsat_proven {false};

    u64 _id_to_add {1};
    std::vector<int> _clause_to_add;
    bool _done_loading {false};

    SipHash _siphash_builder;

public:
    LratChecker(int nbVars, const u8* sigKey128bit = nullptr) :
        _clauses(1<<16),
        _var_values(nbVars+1, 0), _siphash_builder(sigKey128bit) {}

    inline bool loadLiteral(int lit) {
        if (lit == 0) {
            if (!addAxiomaticClause(_id_to_add, _clause_to_add.data(), _clause_to_add.size())) {
                return false;
            }
            _id_to_add++;
            _clause_to_add.push_back(0);
            _siphash_builder.update((u8*) _clause_to_add.data(), _clause_to_add.size()*sizeof(int));
            _clause_to_add.clear();
            return true;
        }
        _clause_to_add.push_back(lit);
        return true;
    }
    bool endLoading(u8*& outSig) {
        if (!_clause_to_add.empty()) {
            snprintf(_errmsg, 512, "literals left in unterminated clause");
            return false;
        }
        outSig = _siphash_builder.digest();
        _done_loading = true;
        return true;
    }

    bool loadOriginalClauses(const int* data, size_t size) {
        u64 id = 1;
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

    inline bool addAxiomaticClause(u64 id, const int* lits, int nbLits) {
        auto [it, ok] = _clauses.insert({id, CompactClause{lits, nbLits}});
        if (!ok) snprintf(_errmsg, 512, "Insertion of clause %lu unsuccessful - already present?", id);
        else if (nbLits == 0) _unsat_proven = true; // added top-level empty clause!
        return ok;
    }
    inline bool addClause(u64 id, const int* lits, int nbLits, const u64* hints, int nbHints) {
        if (!checkClause(id, lits, nbLits, hints, nbHints)) {
            return false;
        }
        return addAxiomaticClause(id, lits, nbLits);
    }
    inline bool deleteClause(const u64* ids, int nbIds) {
        for (int i = 0; i < nbIds; i++) {
            auto id = ids[i];
            auto it = _clauses.find(id);
            if (it == _clauses.end()) {
                snprintf(_errmsg, 512, "Clause deletion: ID %lu not found", id);
                return false;
            }
            _clauses.erase(it);
        }
        return true;
    }
    const char* getErrorMessage() const {
        return _errmsg;
    }

    bool validateUnsat() {
        if (!_done_loading) {
            snprintf(_errmsg, 512, "UNSAT validation illegal - loading formula was not concluded");
            return false;
        }
        if (!_unsat_proven) {
            snprintf(_errmsg, 512, "UNSAT validation unsuccessful - did not derive or import empty clause");
            return false;
        }
        return true;
    }

private:
    inline bool checkClause(u64 baseId, const int* lits, int nbLits, const u64* hints, int nbHints) {
        // Keep track of asserted unit clauses in a stack
        static thread_local std::vector<int> setUnits;

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
            if (MALLOB_UNLIKELY(hintClsIt == _clauses.end())) {
                // ERROR - hint not found
                snprintf(_errmsg, 512, "Derivation %lu: hint %lu not found", baseId, hintId);
                ok = false; break;
            }

            // Interpret hint clause (should derive a new unit clause)
            const auto& hintCls = hintClsIt->second;
            TrustedUtils::doAssert(hintCls.data);
            int newUnit = 0;
            for (int litIdx = 0; ; litIdx++) { // for each literal ...
                int lit = hintCls.data[litIdx];
                if (lit == 0) break;           // ... until termination zero
                int var = abs(lit);
                if (_var_values[var] == 0) {
                    // Literal is unassigned
                    if (MALLOB_UNLIKELY(newUnit != 0)) {
                        // ERROR - multiple unassigned literals in hint clause!
                        snprintf(_errmsg, 512, "Derivation %lu: multiple literals unassigned", baseId);
                        ok = false; break;
                    }
                    newUnit = lit;
                    continue;
                }
                // Literal is fixed
                bool sign = _var_values[var]>0;
                if (MALLOB_UNLIKELY(sign == (lit>0))) {
                    // ERROR - clause is satisfied, so it is not a correct hint
                    snprintf(_errmsg, 512, "Derivation %lu: dependency %lu is satisfied", baseId, hintId);
                    ok = false; break;
                }
                // All OK - literal is false, thus (virtually) removed from the clause
            }
            if (!ok) break; // error detected - stop

            // NO unit derived?
            if (newUnit == 0) {
                // No unassigned literal in the clause && clause not satisfied
                // -> Empty clause derived.
                if (MALLOB_UNLIKELY(i+1 < nbHints)) {
                    // ERROR - not at the final hint yet!
                    snprintf(_errmsg, 512, "Derivation %lu: empty clause produced at non-final hint %lu", baseId, hintId);
                    ok = false; break;
                }
                // Final hint produced empty clause - everything OK!
                for (int var : setUnits) _var_values[var] = 0; // reset variable values
                setUnits.clear();
                return true;
            }
            // Insert the new derived unit clause
            int var = abs(newUnit);
            _var_values[var] = newUnit>0 ? 1 : -1;
            setUnits.push_back(var); // remember to reset later
        }

        // ERROR - something went wrong
        if (_errmsg[0] == '\0')
            snprintf(_errmsg, 512, "Derivation %lu: no empty clause was produced", baseId);
        for (int var : setUnits) _var_values[var] = 0; // reset variable values
        setUnits.clear();
        return false;
    }
};
