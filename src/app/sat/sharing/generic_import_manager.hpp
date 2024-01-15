
#pragma once

#include "app/sat/data/solver_statistics.hpp"
#include "app/sat/execution/solver_setup.hpp"
#include "app/sat/sharing/buffer/buffer_reader.hpp"
#include "app/sat/sharing/store/generic_clause_store.hpp"
#include "util/sys/threading.hpp"
#include <atomic>
#include <functional>

class GenericImportManager {

protected:
    SolverStatistics& _stats;
    std::function<void(Mallob::Clause&)> _preimport_clause_manipulator;
    int _max_clause_length;

    int _imported_revision {0};
    int _solver_revision {0};
    Mutex _mtx_revision;

public:
    GenericImportManager(const SolverSetup& setup, SolverStatistics& stats) : _stats(stats), 
        _max_clause_length(setup.strictClauseLengthLimit) {}
    void setPreimportClauseManipulator(std::function<void(Mallob::Clause&)> cb) {
        _preimport_clause_manipulator = cb;
    }
    virtual ~GenericImportManager() {};

    virtual void addSingleClause(const Mallob::Clause& c) = 0;
    virtual void performImport(BufferReader& reader) = 0;
    void setImportedRevision(int revision) {
        auto lock = _mtx_revision.getLock();
        _imported_revision = revision;
    }
    void updateSolverRevision(int solverRevision) {
        auto lock = _mtx_revision.getLock();
        _solver_revision = solverRevision;
    }
    bool canImport() {
        return _solver_revision >= _imported_revision;
    }
    virtual const std::vector<int>& getUnitsBuffer() = 0;
    Mallob::Clause& getClause(GenericClauseStore::ExportMode mode) {
        auto& cls = get(mode);
        if (_preimport_clause_manipulator) {
            _preimport_clause_manipulator(cls);
        }
        assert(!cls.begin || cls.lbd <= cls.size - ClauseMetadata::numInts()
            || log_return_false("[ERROR] Clause of effective size %i has LBD %i!\n",
            cls.size-ClauseMetadata::numInts(), cls.lbd));
        return cls;
    }

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

protected:
    virtual Mallob::Clause& get(GenericClauseStore::ExportMode mode) = 0;

};
