
#pragma once

#include "app/sat/sharing/filter/generic_clause_filter.hpp"
#include "app/sat/solvers/portfolio_solver_interface.hpp"
#include <vector>
#include <memory>

class GenericExportManager {

protected:
    GenericClauseStore& _clause_store;
    GenericClauseFilter& _filter;
    std::vector<std::shared_ptr<PortfolioSolverInterface>>& _solvers;
    std::vector<SolverStatistics*>& _solver_stats;
    int _max_eff_clause_length;

    ClauseHistogram _hist_failed_filter;
    ClauseHistogram _hist_admitted_to_db;
    ClauseHistogram _hist_dropped_before_db;

public:
    GenericExportManager(GenericClauseStore& clauseStore, GenericClauseFilter& filter,
            std::vector<std::shared_ptr<PortfolioSolverInterface>>& solvers,
            std::vector<SolverStatistics*>& solverStats, int maxEffectiveClauseLength) : 
        _clause_store(clauseStore), _filter(filter), _solvers(solvers), _solver_stats(solverStats),
        _max_eff_clause_length(maxEffectiveClauseLength),
        _hist_failed_filter(maxEffectiveClauseLength), 
        _hist_admitted_to_db(maxEffectiveClauseLength), 
        _hist_dropped_before_db(maxEffectiveClauseLength) {}
    virtual ~GenericExportManager() {}

    virtual void produce(int* begin, int size, int lbd, int producerId, int epoch) = 0;

    ClauseHistogram& getFailedFilterHistogram() {return _hist_failed_filter;}
	ClauseHistogram& getAdmittedHistogram() {return _hist_admitted_to_db;}
	ClauseHistogram& getDroppedHistogram() {return _hist_dropped_before_db;}

protected:
    void handleResult(int producerId, GenericClauseFilter::ExportResult result, int effClauseLength) {
        auto solverStats = _solver_stats.at(producerId);
        if (result == GenericClauseFilter::ADMITTED) {
            _hist_admitted_to_db.increment(effClauseLength);
            if (solverStats) solverStats->producedClausesAdmitted++;
        } else if (result == GenericClauseFilter::FILTERED) {
            _hist_failed_filter.increment(effClauseLength);
            if (solverStats) solverStats->producedClausesFiltered++;
        } else if (result == GenericClauseFilter::DROPPED) {
            _hist_dropped_before_db.increment(effClauseLength);
            if (solverStats) solverStats->producedClausesDropped++;
        }
    }

};
