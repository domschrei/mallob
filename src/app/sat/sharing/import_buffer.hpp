
#pragma once

#include <vector>
#include <list>

#include "buffer/adaptive_clause_database.hpp"
#include "../execution/solver_setup.hpp"

class ImportBuffer {

private:
    SolverStatistics& _stats;
    AdaptiveClauseDatabase _cdb;
    int _max_clause_length;

    std::vector<int> _plain_units_out;
    Mallob::Clause _clause_out;

public:
    ImportBuffer(const SolverSetup& setup, SolverStatistics& stats) : _stats(stats), 
        _cdb([&]() {
            AdaptiveClauseDatabase::Setup cdbSetup;
            cdbSetup.maxClauseLength = setup.strictClauseLengthLimit;
            cdbSetup.maxLbdPartitionedSize = 2;
            cdbSetup.numLiterals = setup.clauseBaseBufferSize * std::max(
                setup.minNumChunksPerSolver, 
                (int) (
                    ((float) setup.numBufferedClsGenerations) * 
                    setup.anticipatedLitsToImportPerCycle / setup.clauseBaseBufferSize
                )
            );
            cdbSetup.slotsForSumOfLengthAndLbd = false;
            cdbSetup.useChecksums = false;
            return cdbSetup;
        }()), _max_clause_length(setup.strictClauseLengthLimit) {}

    int getLiteralBudget(int clauseLength, int lbd) {
        return _cdb.reserveLiteralBudget(clauseLength, lbd);
    }

    template <typename T>
    void performImport(int clauseLength, int lbd, std::forward_list<T>& clauses, int nbLiterals) {
        if (clauses.empty()) return;
        _cdb.addReservedUniformClauses(clauseLength, lbd, clauses, nbLiterals);
    }

    void add(const Mallob::Clause& c) {
        bool success = _cdb.addClause(c);
        if (!success) _stats.receivedClausesDropped++;
    }

    const std::vector<int>& getUnitsBuffer() {

        if (_cdb.getNumLiterals(1, 1) == 0) {
            _plain_units_out.clear();
            return _plain_units_out;
        }

        int numUnits = 0;
        std::vector<int> buf;
        buf = _cdb.exportBuffer(-1, numUnits, AdaptiveClauseDatabase::UNITS, /*sortClauses=*/false);

        _plain_units_out = std::vector<int>(buf.data()+(buf.size()-numUnits), buf.data()+buf.size());
        assert(_plain_units_out.size() == numUnits);
        for (int i = 0; i < _plain_units_out.size(); i++) assert(_plain_units_out[i] != 0);
        _stats.receivedClausesDigested += numUnits;
        _stats.histDigested->increase(1, numUnits);
        return _plain_units_out;
    }

    Mallob::Clause& get(AdaptiveClauseDatabase::ExportMode mode) {

        if (_clause_out.begin != nullptr) {
            free(_clause_out.begin);
            _clause_out.begin = nullptr;
        }

        if (_cdb.getCurrentlyUsedLiterals() == 0) return _clause_out;

        if (_cdb.popFrontWeak(mode, _clause_out)) {
            _stats.receivedClausesDigested++;
            _stats.histDigested->increment(_clause_out.size);
            assert(_clause_out.size > 0);
            assert(_clause_out.lbd > 0);
            assert(_clause_out.begin[0] != 0);
        }

        return _clause_out;
    }

    bool empty() const {
        int litsInUse = _cdb.getCurrentlyUsedLiterals();
        assert(litsInUse >= 0);
        if (litsInUse > 0) return false;
        return true;
    }

    ~ImportBuffer() {
        if (_clause_out.begin != nullptr) free(_clause_out.begin);
    }
};
