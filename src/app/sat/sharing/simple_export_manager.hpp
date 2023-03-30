
#pragma once

#include "app/sat/data/produced_clause_candidate.hpp"
#include "app/sat/sharing/filter/generic_clause_filter.hpp"
#include "app/sat/sharing/store/generic_clause_store.hpp"
#include "app/sat/sharing/generic_export_manager.hpp"

class SimpleExportManager : public GenericExportManager {

public:
    SimpleExportManager(GenericClauseStore& clauseStore, GenericClauseFilter& filter,
            std::vector<std::shared_ptr<PortfolioSolverInterface>>& solvers,
            std::vector<SolverStatistics*>& solverStats, int maxClauseLength) :
        GenericExportManager(clauseStore, filter, solvers, solverStats, maxClauseLength) {}
    virtual ~SimpleExportManager() {}

    void produce(int* begin, int size, int lbd, int producerId, int epoch) override {

        if (size > _max_clause_length
            || size > _clause_store.getMaxAdmissibleClauseLength() 
            || !_filter.tryAcquireLock(size)) {

            handleResult(producerId, GenericClauseFilter::DROPPED, size);
            return;
        }

        ProducedClauseCandidate pcc(begin, size, lbd, producerId, epoch);
        auto result = _filter.tryRegisterAndInsert(std::move(pcc));
        _filter.releaseLock(size);

        auto solverStats = _solver_stats.at(producerId);
        handleResult(producerId, result, size);
    }
};
