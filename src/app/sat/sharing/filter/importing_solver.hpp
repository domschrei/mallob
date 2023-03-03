
#pragma once

#include "app/sat/sharing/clause_id_alignment.hpp"
#include "app/sat/sharing/filter/concurrent_produced_clause_filter.hpp"
#include "app/sat/solvers/portfolio_solver_interface.hpp"

struct ImportingSolver {

    PortfolioSolverInterface* solver;
    SolverStatistics* solverStats;
    std::list<int> unitList;
    std::list<std::pair<int, int>> binaryList;
    std::list<std::vector<int>> largeList;
    AdaptiveClauseDatabase::LinearBudgetCounter budgetCounter;
    int remainingBudget;
    int currentAddedLiterals {0};

    ClauseIdAlignment* _id_alignment {nullptr};
    
    ImportingSolver(PortfolioSolverInterface* solver, SolverStatistics* stats, ClauseIdAlignment* idAlignment) : 
        solver(solver), solverStats(stats),
        budgetCounter(solver->getImportBudgetCounter()),
        remainingBudget(budgetCounter.getTotalBudget() - budgetCounter.getNextOccupiedBudget(1, 1)),
        _id_alignment(idAlignment) {}
    
    void publishPreparedLists(BufferIterator& it) {
        if (!unitList.empty()) {
            solver->addLearnedClauses(it.clauseLength, it.lbd, unitList, currentAddedLiterals);
        } else if (!binaryList.empty()) {
            solver->addLearnedClauses(it.clauseLength, it.lbd, binaryList, currentAddedLiterals);
        } else if (!largeList.empty()) {
            solver->addLearnedClauses(it.clauseLength, it.lbd, largeList, currentAddedLiterals);
        }
    }

    void initializeNextSlot(int clauseLength, int lbd) {
        currentAddedLiterals = 0;
        remainingBudget -= budgetCounter.getNextOccupiedBudget(clauseLength, lbd);
    }

    void tryImport(const Mallob::Clause& clause, cls_producers_bitset producers, bool explicitLbds) {

        int sid = solver->getLocalId();
        solverStats->receivedClauses++;
        if (remainingBudget < clause.size) {
            // No import budget left
            solverStats->receivedClausesDropped++;
            return;
        }
        cls_producers_bitset producerFlag = 1 << sid;
        if ((producers & producerFlag) != 0) {
            // filtered by solver filter
            solverStats->receivedClausesFiltered++;
            return;
        }
        if (_id_alignment && !_id_alignment->checkClauseToImport(solver, clause)) {
            // filtered by having an ID produced by this solver itself
            solverStats->receivedClausesFiltered++;
            return;
        }
        // admitted by solver filter
        if (clause.size == 1) unitList.push_front(clause.begin[0]);
        else if (clause.size == 2) binaryList.emplace_front(clause.begin[0], clause.begin[1]);
        else {
            std::vector<int> clauseVec((explicitLbds ? 1 : 0) + clause.size);
            size_t idx = 0;
            if (explicitLbds) clauseVec[idx++] = clause.lbd;
            for (size_t k = 0; k < clause.size; k++) clauseVec[idx++] = clause.begin[k];
            largeList.emplace_front(std::move(clauseVec));
        }
        remainingBudget -= clause.size;
        currentAddedLiterals += clause.size;
    }
};
