
#pragma once

#include "siphash/siphash.hpp"
#include "robin_map.h"
#include "trusted_utils.hpp"

class LratChecker {

private:
    struct CompactClause {
        int* data {nullptr};
        int nbLits = 0;
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
        bool operator==(const CompactClause& other) const {
            if (nbLits != other.nbLits) return false;
            for (int i = 0; i < nbLits; i++) if (data[i] != other.data[i]) return false;
            return true;
        }
        bool operator!=(const CompactClause& other) const {
            return !(*this == other);
        }
        ~CompactClause() {
            if (data) free(data);
        }
    };
    struct ClauseIdHasher {
        std::size_t operator()(const uint64_t& val) const {
            return (0xcbf29ce484222325UL ^ val) * 0x00000100000001B3UL;
        }
    };
    tsl::robin_map<uint64_t, CompactClause, ClauseIdHasher> _clauses;
    std::vector<int8_t> _var_values;
    const char* _errmsg = nullptr;
    bool _unsat_proven {false};

    uint64_t _id_to_add {1};
    std::vector<int> _clause_to_add;
    bool _done_loading {false};

    SipHash _siphash_builder;

public:
    LratChecker(int nbVars, const uint8_t* sigKey128bit = nullptr) :
        _clauses(1<<16),
        _var_values(nbVars+1, 0), _siphash_builder(sigKey128bit) {}

    inline bool loadLiteral(int lit) {
        if (lit == 0) {
            if (!addAxiomaticClause(_id_to_add, _clause_to_add.data(), _clause_to_add.size())) {
                return false;
            }
            _id_to_add++;
            _clause_to_add.push_back(0);
            _siphash_builder.update((uint8_t*) _clause_to_add.data(), _clause_to_add.size()*sizeof(int));
            _clause_to_add.clear();
            return true;
        }
        _clause_to_add.push_back(lit);
        return true;
    }
    bool endLoading(uint8_t*& outSig) {
        if (!_clause_to_add.empty()) {
            _errmsg = "literals left in unterminated clause";
            return false;
        }
        outSig = _siphash_builder.digest();
        _done_loading = true;
        return true;
    }

    bool loadOriginalClauses(const int* data, size_t size) {
        uint64_t id = 1;
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

    inline bool addAxiomaticClause(uint64_t id, const int* lits, int nbLits) {
        //LOG(V2_INFO, "IMPORT %lu\n", id);
        auto [it, ok] = _clauses.insert({id, CompactClause{lits, nbLits}});
        if (!ok) _errmsg = "Unsuccessful insertion of clause";
        else if (nbLits == 0) _unsat_proven = true; // added top-level empty clause!
        return ok;
    }
    inline bool addClause(uint64_t id, const int* lits, int nbLits, const uint64_t* hints, int nbHints) {
        //LOG(V2_INFO, "PRODUCE %lu\n", id);
        if (!checkClause(lits, nbLits, hints, nbHints)) {
            return false;
        }
        return addAxiomaticClause(id, lits, nbLits);
    }
    inline bool deleteClause(const uint64_t* ids, int nbIds) {
        for (int i = 0; i < nbIds; i++) {
            auto id = ids[i];
            //printf("PROOF?? DELETE %lu\n", id);
            auto it = _clauses.find(id);
            if (it == _clauses.end()) {
                // ERROR
                _errmsg = "Clause deletion: ID not found";
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
    inline bool checkClause(const int* lits, int nbLits, const uint64_t* hints, int nbHints) {
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
            if (hintClsIt == _clauses.end()) {
                // ERROR - hint not found
                _errmsg = "Clause addition: ID not found";
                ok = false; break;
            }

            // Interpret hint clause (should derive a new unit clause)
            const auto& hintCls = hintClsIt->second;
            //std::string lits; for (int i = 0; i < hintCls.nbLits; i++) lits += std::to_string(hintCls.data[i]) + " ";
            //printf("PROOF?? %lu : %s\n", hintId, lits.c_str());
            int newUnit = 0;
            for (int litIdx = 0; litIdx < hintCls.nbLits; litIdx++) {
                int lit = hintCls.data[litIdx];
                int var = abs(lit);
                if (_var_values[var] == 0) {
                    // Literal is unassigned
                    if (newUnit != 0) {
                        // ERROR - multiple unassigned literals in hint clause!
                        _errmsg = "Multiple literals unassigned";
                        ok = false; break;
                    }
                    newUnit = lit;
                    continue;
                }
                // Literal is fixed
                bool sign = _var_values[var]>0;
                if (sign == (lit>0)) {
                    // ERROR - clause is satisfied, so it does not belong to hints
                    _errmsg = "Clause hint is satisfied";
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
                    _errmsg = "Empty clause produced at non-final hint";
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
        if (!_errmsg) _errmsg = "No empty clause was produced";
        for (int var : setUnits) _var_values[var] = 0; // reset variable values
        setUnits.clear();
        return false;
    }
};
