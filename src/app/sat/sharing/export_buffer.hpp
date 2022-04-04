
#pragma once

#include <list>

#include "util/sys/threading.hpp"
#include "filter/produced_clause_filter.hpp"
#include "buffer/adaptive_clause_database.hpp"
#include "../data/solver_statistics.hpp"

class ExportBuffer {

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
    ExportBuffer(ProducedClauseFilter& filter, AdaptiveClauseDatabase& cdb, 
            std::vector<SolverStatistics*>& solverStats, int maxClauseLength) : 
        _filter(filter), _cdb(cdb), _solver_stats(solverStats),
        _hist_failed_filter(maxClauseLength), 
        _hist_admitted_to_db(maxClauseLength), 
        _hist_dropped_before_db(maxClauseLength) {}

    void produce(int* begin, int size, int lbd, int producerId, int epoch) {

        if (_filter.tryAcquireLock()) {

            // Insert clause directly
            auto result = _filter.tryRegisterAndInsert(
                ProducedClauseCandidate(begin, size, lbd, producerId, epoch), 
                _cdb
            );
            handleResult(producerId, result, size);

            // Also decrease backlog size by some amount
            std::list<ProducedClauseCandidate> backlogSplice;
            {
                auto lock = _backlog_mutex.getLock();
                if (!_export_backlog.empty()) {
                    backlogSplice.splice(backlogSplice.begin(), _export_backlog, 
                        _export_backlog.begin(),
                        std::next(_export_backlog.begin(), std::min(16ul, _export_backlog.size()))
                    );
                }
            }
            for (auto& pcc : backlogSplice) {
                int clauseLength = pcc.size;
                int producerId = pcc.producerId;
                result = _filter.tryRegisterAndInsert(std::move(pcc), _cdb);
                handleResult(producerId, result, clauseLength);
            }

            _filter.releaseLock();

        } else {

            // Filter busy: Append clause to backlog
            auto lock = _backlog_mutex.getLock();
            _export_backlog.emplace_back(begin, size, lbd, producerId, epoch);        
        }
    }

    ClauseHistogram& getFailedFilterHistogram() {return _hist_failed_filter;}
	ClauseHistogram& getAdmittedHistogram() {return _hist_admitted_to_db;}
	ClauseHistogram& getDroppedHistogram() {return _hist_dropped_before_db;}

private:
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
