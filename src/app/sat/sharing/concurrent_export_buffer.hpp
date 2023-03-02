
#pragma once

#include <list>

#include "app/sat/data/produced_clause_candidate.hpp"
#include "util/sys/threading.hpp"
#include "filter/concurrent_produced_clause_filter.hpp"
#include "buffer/adaptive_clause_database.hpp"
#include "../data/solver_statistics.hpp"

class ConcurrentExportBuffer {

private:
    ConcurrentProducedClauseFilter& _filter;
    AdaptiveClauseDatabase& _cdb;
    std::vector<SolverStatistics*>& _solver_stats;

    ClauseHistogram _hist_failed_filter;
	ClauseHistogram _hist_admitted_to_db;
	ClauseHistogram _hist_dropped_before_db;

public:
    ConcurrentExportBuffer(ConcurrentProducedClauseFilter& filter, AdaptiveClauseDatabase& cdb, 
            std::vector<SolverStatistics*>& solverStats, int maxClauseLength) : 
        _filter(filter), _cdb(cdb), _solver_stats(solverStats),
        _hist_failed_filter(maxClauseLength), 
        _hist_admitted_to_db(maxClauseLength), 
        _hist_dropped_before_db(maxClauseLength) {}

    void produce(int* begin, int size, int lbd, int producerId, int epoch) {

        // Insert clause directly
        ProducedClauseCandidate pcc(begin, size, lbd, producerId, epoch);
        processClause(pcc);
    }

    ClauseHistogram& getFailedFilterHistogram() {return _hist_failed_filter;}
	ClauseHistogram& getAdmittedHistogram() {return _hist_admitted_to_db;}
	ClauseHistogram& getDroppedHistogram() {return _hist_dropped_before_db;}

private:
    void processClause(ProducedClauseCandidate& pcc) {
        int clauseLength = pcc.size;
        int producerId = pcc.producerId;
        auto result = _filter.tryRegisterAndInsert(std::move(pcc));
        handleResult(producerId, result, clauseLength);
    }

    void handleResult(int producerId, ConcurrentProducedClauseFilter::ExportResult result, int clauseLength) {
        auto solverStats = _solver_stats.at(producerId);
        if (result == ConcurrentProducedClauseFilter::ADMITTED) {
            _hist_admitted_to_db.increment(clauseLength);
            if (solverStats) solverStats->producedClausesAdmitted++;
        } else if (result == ConcurrentProducedClauseFilter::FILTERED) {
            _hist_failed_filter.increment(clauseLength);
            if (solverStats) solverStats->producedClausesFiltered++;
        } else if (result == ConcurrentProducedClauseFilter::DROPPED) {
            _hist_dropped_before_db.increment(clauseLength);
            if (solverStats) solverStats->producedClausesDropped++;
        }
    }

};
