
#pragma once

#include <list>

#include "app/sat/data/produced_clause_candidate.hpp"
#include "app/sat/sharing/buffer/priority_clause_buffer.hpp"
#include "util/logger.hpp"
#include "util/sys/threading.hpp"
#include "filter/concurrent_produced_clause_filter.hpp"
#include "buffer/adaptive_clause_database.hpp"
#include "../data/solver_statistics.hpp"

class ConcurrentExportBuffer {

private:
    struct Slot {
        int clauseLength;
        ConcurrentProducedClauseFilter filter;
        Mutex mtxBacklog;
        std::list<ProducedClauseCandidate> backlog;

        Slot(PriorityClauseBuffer& pcb, int epochHorizon, bool reshareImprovedLbd, int clauseLength) :
            clauseLength(clauseLength), filter(pcb, epochHorizon, reshareImprovedLbd) {}
    };

    std::vector<std::unique_ptr<Slot>> _slots;
    PriorityClauseBuffer& _pcb;
    std::vector<SolverStatistics*>& _solver_stats;

    ClauseHistogram _hist_failed_filter;
    ClauseHistogram _hist_admitted_to_db;
    ClauseHistogram _hist_dropped_before_db;

public:
    ConcurrentExportBuffer(PriorityClauseBuffer& pcb, int epochHorizon, bool reshareImprovedLbd,
            std::vector<SolverStatistics*>& solverStats, int maxClauseLength) : 
        _pcb(pcb), _solver_stats(solverStats),
        _hist_failed_filter(maxClauseLength), 
        _hist_admitted_to_db(maxClauseLength), 
        _hist_dropped_before_db(maxClauseLength) {

        _slots.resize(maxClauseLength+1);
        for (size_t i = 0; i < _slots.size(); i++)
            _slots[i].reset(new Slot(pcb, epochHorizon, reshareImprovedLbd, i+1));
    }

    void produce(int* begin, int size, int lbd, int producerId, int epoch) {

        ProducedClauseCandidate pcc(begin, size, lbd, producerId, epoch);

        auto& slot = getSlot(size);
        auto& filter = slot.filter;
        auto& mtxBacklog = slot.mtxBacklog;
        auto& backlog = slot.backlog;

        // Can I expect to quickly obtain the map's internal locks?
        if (filter.tryGetSharedLock()) {
            // -- yes!

            // Insert clause directly
            processClause(pcc, filter);

            // Reduce backlog size
            std::list<ProducedClauseCandidate> extracted;
            {
                auto lock = mtxBacklog.getLock();
                auto endIt = backlog.begin();
                std::advance(endIt, std::min(32UL, backlog.size()));
                extracted.splice(extracted.end(), backlog, backlog.begin(), endIt);
            }
            while (!extracted.empty()) {
                processClause(extracted.front(), filter);
                backlog.pop_front();
            }

            filter.returnSharedLock();

        } else {
            // -- no: Insert into backlog
            auto lock = mtxBacklog.getLock();
            backlog.push_back(std::move(pcc));
        }
    }

    ConcurrentProducedClauseFilter& getFilter(int clauseLength) {
        return getSlot(clauseLength).filter;
    }

    void updateEpoch(int epoch) {
        for (auto& slot : _slots) slot->filter.updateEpoch(epoch);
    }

    void collectGarbage() {
        for (auto& slot : _slots) slot->filter.collectGarbage();
    }

    ClauseHistogram& getFailedFilterHistogram() {return _hist_failed_filter;}
	ClauseHistogram& getAdmittedHistogram() {return _hist_admitted_to_db;}
	ClauseHistogram& getDroppedHistogram() {return _hist_dropped_before_db;}

private:
    Slot& getSlot(int clauseLength) {return *_slots.at(clauseLength-1);}

    void processClause(ProducedClauseCandidate& pcc, ConcurrentProducedClauseFilter& filter) {
        int clauseLength = pcc.size;
        int producerId = pcc.producerId;
        auto result = filter.tryRegisterAndInsert(std::move(pcc));
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
