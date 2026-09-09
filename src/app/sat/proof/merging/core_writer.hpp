
#pragma once

#include "app/sat/parse/cnf_util.hpp"
#include "app/sat/proof/serialized_lrat_line.hpp"
#include "robin_set.h"

class CoreWriter {

private:
    const std::vector<std::vector<int>> _ordered_clauses;
    tsl::robin_set<int> _core_clause_indices;
    std::string _out_path;

public:
    CoreWriter(std::vector<std::vector<int>>&& clauses, const std::string& outPath)
        : _ordered_clauses(std::move(clauses)), _out_path(outPath) {}

    void pushAddition(const SerializedLratLine& line) {
        auto [data, nbHints] = line.getHints();
        for (int i = 0; i < nbHints; i++) {
            uint64_t hint = data[i];
            assert(hint > 0);
            if (hint < _ordered_clauses.size()) {
                // original clause being referenced
                _core_clause_indices.insert((int) hint);
            }
        }
    }

    void output() {

        std::vector<int> linearIndices(_core_clause_indices.begin(), _core_clause_indices.end());
        std::sort(linearIndices.begin(), linearIndices.end());
        const int nbClauses = linearIndices.size();

        std::vector<int> outputLiterals;
        int maxVar = 1;
        for (int idx : linearIndices) {
            auto& cls = _ordered_clauses[idx];
            for (int lit : cls) {
                maxVar = std::max(maxVar, std::abs(lit));
                outputLiterals.push_back(lit);
            }
            outputLiterals.push_back(0);
        }

        outputLiterals.push_back(maxVar);
        outputLiterals.push_back(nbClauses);
        CnfUtil::writeFormula(outputLiterals, _out_path);

        _core_clause_indices.clear();
    }

    ~CoreWriter() {
        output();
    }
};
