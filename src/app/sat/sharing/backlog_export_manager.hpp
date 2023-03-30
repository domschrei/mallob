
#pragma once

#include <list>

#include "app/sat/data/produced_clause_candidate.hpp"
#include "app/sat/sharing/filter/generic_clause_filter.hpp"
#include "app/sat/sharing/generic_export_manager.hpp"
#include "app/sat/sharing/store/generic_clause_store.hpp"
#include "app/sat/solvers/portfolio_solver_interface.hpp"
#include "util/logger.hpp"
#include "util/sys/threading.hpp"
#include "../data/solver_statistics.hpp"
#include "util/sys/timer.hpp"

class BacklogExportManager : public GenericExportManager {

private:
    struct Slot {
        int clauseLength;
        Mutex mtxBacklog;
        std::list<ProducedClauseCandidate> backlog;
        float lastBacklogWarn {0};

        Slot(GenericClauseStore& pcb, int clauseLength) : clauseLength(clauseLength) {}
    };

    std::vector<std::unique_ptr<Slot>> _slots;

public:
    BacklogExportManager(GenericClauseStore& pcb, GenericClauseFilter& filter,
            std::vector<std::shared_ptr<PortfolioSolverInterface>>& solvers,
            std::vector<SolverStatistics*>& solverStats, int maxClauseLength) :
        GenericExportManager(pcb, filter, solvers, solverStats, maxClauseLength) {

        _slots.resize(maxClauseLength);
        for (size_t i = 0; i < _slots.size(); i++)
            _slots[i].reset(new Slot(pcb, i+1));
    }
    virtual ~BacklogExportManager() {}

    void produce(int* begin, int size, int lbd, int producerId, int epoch) override {

        ProducedClauseCandidate pcc(begin, size, lbd, producerId, epoch);

        if (size > _clause_store.getMaxAdmissibleClauseLength()) {
            handleResult(producerId, GenericClauseFilter::DROPPED, size);
            return;
        }

        auto& slot = getSlot(size);
        auto& mtxBacklog = slot.mtxBacklog;
        auto& backlog = slot.backlog;

        // Can I expect to quickly obtain the map's internal locks?
        if (_filter.tryAcquireLock(size)) {
            // -- yes!

            // Insert clause directly
            processClause(pcc, true);

            // Reduce backlog size
            std::list<ProducedClauseCandidate> extracted;
            size_t backlogSize;
            bool warnSize = false;
            {
                auto lock = mtxBacklog.getLock();
                auto endIt = backlog.begin();
                std::advance(endIt, std::min(32UL, backlog.size()));
                extracted.splice(extracted.end(), backlog, backlog.begin(), endIt);
                backlogSize = backlog.size();
                if (backlogSize >= (1<<16)) {
                    auto time = Timer::elapsedSeconds();
                    if (time - slot.lastBacklogWarn >= 1.0) {
                        warnSize = true;
                        slot.lastBacklogWarn = time;
                    }
                }
            }
            while (!extracted.empty()) {
                processClause(extracted.front());
                extracted.pop_front();
            }

            _filter.releaseLock(size);

            // Print a warning periodically if the backlog is very large
            if (warnSize && _solvers[producerId]) {
                LOGGER(_solvers[producerId]->getLogger(), V1_WARN, "[WARN] Export backlog for clauses of len %i had size %lu\n",
                    size, backlogSize);
            }

        } else {
            // -- no: Insert into backlog
            auto lock = mtxBacklog.getLock();
            backlog.push_back(std::move(pcc));
        }
    }

private:
    Slot& getSlot(int clauseLength) {return *_slots.at(clauseLength-1);}

    void processClause(ProducedClauseCandidate& pcc, bool checkedForAdmissibleClauseLength = false) {
        int clauseLength = pcc.size;
        int producerId = pcc.producerId;
        if (!checkedForAdmissibleClauseLength && clauseLength > _clause_store.getMaxAdmissibleClauseLength()) {
            handleResult(producerId, GenericClauseFilter::DROPPED, clauseLength);
            return;
        }
        auto result = _filter.tryRegisterAndInsert(std::move(pcc));
        handleResult(producerId, result, clauseLength);
    }
};
