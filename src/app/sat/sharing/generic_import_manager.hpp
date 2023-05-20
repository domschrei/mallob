
#pragma once

#include "app/sat/data/solver_statistics.hpp"
#include "app/sat/execution/solver_setup.hpp"
#include "app/sat/sharing/buffer/buffer_reader.hpp"
#include "app/sat/sharing/store/generic_clause_store.hpp"

class GenericImportManager {

protected:
    SolverStatistics& _stats;
    int _max_clause_length;
    bool _reset_lbd;
    bool _increment_lbd;

public:
    GenericImportManager(const SolverSetup& setup, SolverStatistics& stats) : _stats(stats), 
        _max_clause_length(setup.strictClauseLengthLimit),
        _reset_lbd(setup.resetLbdBeforeImport),
        _increment_lbd(setup.incrementLbdBeforeImport) {}
    virtual ~GenericImportManager() {};

    virtual void addSingleClause(const Mallob::Clause& c) = 0;
    virtual void performImport(BufferReader& reader) = 0;

    virtual const std::vector<int>& getUnitsBuffer() = 0;
    virtual Mallob::Clause& get(GenericClauseStore::ExportMode mode) = 0;

    virtual bool empty() const {
        return size() == 0;
    }
    virtual size_t size() const = 0;

    int getLiteralBudget(const SolverSetup& setup) {
        return setup.clauseBaseBufferSize * std::max(
            setup.minNumChunksPerSolver, 
            (int) (
                ((float) setup.numBufferedClsGenerations) * 
                setup.anticipatedLitsToImportPerCycle / setup.clauseBaseBufferSize
            )
        );
    }
};
