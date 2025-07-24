
#pragma once

#include "app/sat/sharing/clause_id_alignment.hpp"
#include "app/sat/sharing/filter/produced_clause_filter_commons.hpp"
#include "app/sat/solvers/portfolio_solver_interface.hpp"

struct ImportingSolver {

    const int globalId;
    const int localId;
    SolverStatistics* solverStats;
    std::vector<bool> filter;

    ClauseIdAlignment* _id_alignment {nullptr};
    
    ImportingSolver(int globalId, int localId, SolverStatistics* stats, ClauseIdAlignment* idAlignment) : 
        globalId(globalId), localId(localId), solverStats(stats), _id_alignment(idAlignment) {}

    void appendCandidate(const Mallob::Clause& clause, cls_producers_bitset producers) {
        filter.push_back(filterClause(clause, producers));
    }

private:
    bool filterClause(const Mallob::Clause& clause, cls_producers_bitset producers) {

        solverStats->receivedClauses++;
        //if (remainingBudget < clause.size) {
        //    // No import budget left
        //    solverStats->receivedClausesDropped++;
        //    return;
        //}
        cls_producers_bitset producerFlag = 1 << localId;
        if ((producers & producerFlag) != 0) {
            // filtered by solver filter
            solverStats->receivedClausesFiltered++;
            return true;
        }
        if (_id_alignment && !_id_alignment->checkClauseToImport(globalId, localId, clause)) {
            // filtered by having an ID produced by this solver itself
            solverStats->receivedClausesFiltered++;
            return true;
        }
        // admitted by solver filter
        return false;
    }
};
