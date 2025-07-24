
#pragma once

#include <cstddef>

#include "app/sat/data/clause.hpp"
#include "util/params.hpp"

class ClausePrefilter {

private:
    const Parameters& _params;
    // Additional fields and helper structs could go here

public:
    ClausePrefilter(const Parameters& params) : _params(params) {}

    // Called whenever a new increment of a formula arrives.
    // (Only once if we're doing normal, non-incremental solving.)
    void notifyFormula(const int* data, size_t size) {
        // If needed, the prefilter can use the formula data here for precomputations.
    }

    // The prefilter may manipulate the clause's quality level (clause.lbd)
    // and needs to indicate whether to accept or drop the clause.
    bool prefilterClause(Mallob::Clause& clause) {
        return true; // accept all
    }
};
