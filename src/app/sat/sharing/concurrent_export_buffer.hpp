
#pragma once

#include <list>

#include "app/sat/data/produced_clause_candidate.hpp"
#include "util/logger.hpp"
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

    Mutex _mtx_backlog;
    std::list<ProducedClauseCandidate> _backlog;

public:
    ConcurrentExportBuffer(ConcurrentProducedClauseFilter& filter, AdaptiveClauseDatabase& cdb, 
            std::vector<SolverStatistics*>& solverStats, int maxClauseLength) : 
        _filter(filter), _cdb(cdb), _solver_stats(solverStats),
        _hist_failed_filter(maxClauseLength), 
        _hist_admitted_to_db(maxClauseLength), 
        _hist_dropped_before_db(maxClauseLength) {}

    void produce(int* begin, int size, int lbd, int producerId, int epoch) {

        ProducedClauseCandidate pcc(begin, size, lbd, producerId, epoch);

        // Can I expect to quickly obtain the map's internal locks?
        if (_filter.tryGetSharedLock()) {
            // -- yes!

            // Insert clause directly
            processClause(pcc);

            // Reduce backlog size
            {
                auto lock = _mtx_backlog.getLock();
                int nbProcessed = 0;
                while (!_backlog.empty() && nbProcessed < 10) {
                    processClause(_backlog.front());
                    _backlog.pop_front();
                    nbProcessed++;
                }
                if (nbProcessed > 0) {
                    LOG(V2_INFO, "Reduced backlog size from %i to %i\n", 
                        _backlog.size()+nbProcessed, _backlog.size());
                }
            }

            _filter.returnSharedLock();

        } else {
            // -- no: Insert into backlog
            auto lock = _mtx_backlog.getLock();
            _backlog.push_back(std::move(pcc));
        }
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
