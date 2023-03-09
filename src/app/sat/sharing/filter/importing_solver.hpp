
#pragma once

#include "app/sat/sharing/buffer/buffer_reader.hpp"
#include "app/sat/sharing/clause_id_alignment.hpp"
#include "app/sat/sharing/filter/concurrent_produced_clause_filter.hpp"
#include "app/sat/solvers/portfolio_solver_interface.hpp"

struct ImportingSolver {

    PortfolioSolverInterface* solver;
    SolverStatistics* solverStats;
    std::vector<bool> filter;

    ClauseIdAlignment* _id_alignment {nullptr};
    
    ImportingSolver(PortfolioSolverInterface* solver, SolverStatistics* stats, ClauseIdAlignment* idAlignment) : 
        solver(solver), solverStats(stats), _id_alignment(idAlignment) {}

    void appendCandidate(const Mallob::Clause& clause, cls_producers_bitset producers) {
        filter.push_back(filterClause(clause, producers));
    }

private:
    bool filterClause(const Mallob::Clause& clause, cls_producers_bitset producers) {

        int sid = solver->getLocalId();
        solverStats->receivedClauses++;
        //if (remainingBudget < clause.size) {
        //    // No import budget left
        //    solverStats->receivedClausesDropped++;
        //    return;
        //}
        cls_producers_bitset producerFlag = 1 << sid;
        if ((producers & producerFlag) != 0) {
            // filtered by solver filter
            solverStats->receivedClausesFiltered++;
            return true;
        }
        if (_id_alignment && !_id_alignment->checkClauseToImport(solver, clause)) {
            // filtered by having an ID produced by this solver itself
            solverStats->receivedClausesFiltered++;
            return true;
        }
        // admitted by solver filter
        return false;
    }
};
