
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
    int _max_clause_length;

    ClauseHistogram _hist_failed_filter;
    ClauseHistogram _hist_admitted_to_db;
    ClauseHistogram _hist_dropped_before_db;

public:
    GenericExportManager(GenericClauseStore& clauseStore, GenericClauseFilter& filter,
            std::vector<std::shared_ptr<PortfolioSolverInterface>>& solvers,
            std::vector<SolverStatistics*>& solverStats, int maxClauseLength) : 
        _clause_store(clauseStore), _filter(filter), _solvers(solvers), _solver_stats(solverStats),
        _max_clause_length(maxClauseLength),
        _hist_failed_filter(maxClauseLength), 
        _hist_admitted_to_db(maxClauseLength), 
        _hist_dropped_before_db(maxClauseLength) {}
    virtual ~GenericExportManager() {}

    virtual void produce(int* begin, int size, int lbd, int producerId, int epoch) = 0;

    ClauseHistogram& getFailedFilterHistogram() {return _hist_failed_filter;}
	ClauseHistogram& getAdmittedHistogram() {return _hist_admitted_to_db;}
	ClauseHistogram& getDroppedHistogram() {return _hist_dropped_before_db;}

protected:
    void handleResult(int producerId, GenericClauseFilter::ExportResult result, int clauseLength) {
        auto solverStats = _solver_stats.at(producerId);
        if (result == GenericClauseFilter::ADMITTED) {
            _hist_admitted_to_db.increment(clauseLength);
            if (solverStats) solverStats->producedClausesAdmitted++;
        } else if (result == GenericClauseFilter::FILTERED) {
            _hist_failed_filter.increment(clauseLength);
            if (solverStats) solverStats->producedClausesFiltered++;
        } else if (result == GenericClauseFilter::DROPPED) {
            _hist_dropped_before_db.increment(clauseLength);
            if (solverStats) solverStats->producedClausesDropped++;
        }
    }

};
