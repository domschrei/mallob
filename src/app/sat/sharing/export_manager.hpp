
#pragma once

#include <list>

#include "app/sat/data/produced_clause_candidate.hpp"
#include "util/sys/threading.hpp"
#include "filter/produced_clause_filter.hpp"
#include "../data/solver_statistics.hpp"

class ExportManager {

private:
    Mutex _backlog_mutex;
    std::list<ProducedClauseCandidate> _export_backlog;

    ProducedClauseFilter& _filter;
    AdaptiveClauseDatabase& _cdb;
    std::vector<SolverStatistics*>& _solver_stats;

    ClauseHistogram _hist_failed_filter;
	ClauseHistogram _hist_admitted_to_db;
	ClauseHistogram _hist_dropped_before_db;

public:
    ExportManager(ProducedClauseFilter& filter, AdaptiveClauseDatabase& cdb, 
            std::vector<SolverStatistics*>& solverStats, int maxClauseLength) : 
        _filter(filter), _cdb(cdb), _solver_stats(solverStats),
        _hist_failed_filter(maxClauseLength), 
        _hist_admitted_to_db(maxClauseLength), 
        _hist_dropped_before_db(maxClauseLength) {}

    void produce(int* begin, int size, int lbd, int producerId, int epoch) {

        if (_filter.tryAcquireLock()) {

            // Insert clause directly
            ProducedClauseCandidate pcc(begin, size, lbd, producerId, epoch);
            processClause(pcc);

            // Also process the backlog of clauses which couldn't be processed before
            // since the lock couldn't be obtained
            std::list<ProducedClauseCandidate> backlogSplice;
            {
                // Extract the entire backlog to your local structure (O(1))
                auto lock = _backlog_mutex.getLock();
                backlogSplice = std::move(_export_backlog);
            }
            // Process each clause from the extracted backlog
            for (auto& pcc : backlogSplice) {
                processClause(pcc);
            }

            _filter.releaseLock();

        } else {

            // Filter busy: Append clause to backlog (mutex only guards O(1) ops)
            auto lock = _backlog_mutex.getLock();
            _export_backlog.emplace_back(begin, size, lbd, producerId, epoch);
        }
    }

    ClauseHistogram& getFailedFilterHistogram() {return _hist_failed_filter;}
	ClauseHistogram& getAdmittedHistogram() {return _hist_admitted_to_db;}
	ClauseHistogram& getDroppedHistogram() {return _hist_dropped_before_db;}

private:
    void processClause(ProducedClauseCandidate& pcc) {
        int clauseLength = pcc.size;
        int producerId = pcc.producerId;
        auto result = _filter.tryRegisterAndInsert(std::move(pcc), _cdb);
        handleResult(producerId, result, clauseLength);
    }

    void handleResult(int producerId, ProducedClauseFilter::ExportResult result, int clauseLength) {
        auto solverStats = _solver_stats.at(producerId);
        if (result == ProducedClauseFilter::ADMITTED) {
            _hist_admitted_to_db.increment(clauseLength);
            if (solverStats) solverStats->producedClausesAdmitted++;
        } else if (result == ProducedClauseFilter::FILTERED) {
            _hist_failed_filter.increment(clauseLength);
            if (solverStats) solverStats->producedClausesFiltered++;
        } else if (result == ProducedClauseFilter::DROPPED) {
            _hist_dropped_before_db.increment(clauseLength);
            if (solverStats) solverStats->producedClausesDropped++;
        }
    }

};
