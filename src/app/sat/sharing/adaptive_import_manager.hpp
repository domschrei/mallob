
#pragma once

#include <vector>
#include <list>

#include "app/sat/data/clause.hpp"
#include "app/sat/data/clause_histogram.hpp"
#include "app/sat/data/clause_metadata.hpp"
#include "app/sat/sharing/buffer/buffer_reader.hpp"
#include "app/sat/sharing/generic_import_manager.hpp"
#include "store/adaptive_clause_store.hpp"
#include "../execution/solver_setup.hpp"
#include "util/logger.hpp"

class AdaptiveImportManager : public GenericImportManager {

private:
    AdaptiveClauseStore _pcb;

    std::vector<int> _plain_units_out;
    std::vector<int> _clause_out_data;
    Mallob::Clause _clause_out;

public:
    AdaptiveImportManager(const SolverSetup& setup, SolverStatistics& stats) :
        GenericImportManager(setup, stats), 
        _pcb([&]() {
            AdaptiveClauseStore::Setup pcbSetup;
            pcbSetup.maxEffectiveClauseLength = setup.strictMaxLitsPerClause+ClauseMetadata::numInts();
            pcbSetup.maxFreeEffectiveClauseLength = setup.freeMaxLitsPerClause+ClauseMetadata::numInts();
            pcbSetup.maxLbdPartitionedSize = 2;
            pcbSetup.numLiterals = getLiteralBudget(setup);
            pcbSetup.slotsForSumOfLengthAndLbd = false;
            pcbSetup.useChecksums = false;
            return pcbSetup;
        }()),
        _clause_out_data(setup.strictMaxLitsPerClause+ClauseMetadata::numInts()),
        _clause_out(_clause_out_data.data(), 0, 0) {

        for (int clslen = 1; clslen <= setup.strictMaxLitsPerClause+ClauseMetadata::numInts(); clslen++) {
            _pcb.setClauseDeletionCallback(clslen, [&](Mallob::Clause& cls) {
                _stats.receivedClausesDropped++;
            });
        }
    }

    void performImport(BufferReader& reader) override {
        //LOG(V2_INFO, "DBG perform import of size %i\n", reader.getRemainingSize());
        _pcb.addClauses(reader, nullptr);
        LOG(V5_DEBG, "%lu lits in import buffer: %s dropped:%i\n", size(), _pcb.getCurrentlyUsedLiteralsReport().c_str(), _stats.receivedClausesDropped);
        LOG(V5_DEBG, "clenhist imp_disc %s\n", _pcb.getDeletedClausesHistogram().getReport().c_str());
    }

    void addSingleClause(const Mallob::Clause& c) override {
        if (ClauseMetadata::enabled()) {
            // Perform safety checks
            assert(c.size >= 3);
        }
        bool success = _pcb.addClause(c);
        if (!success) _stats.receivedClausesDropped++;
    }

    const std::vector<int>& getUnitsBuffer() override {

        if (_pcb.getNumLiterals(1, 1) == 0) {
            _plain_units_out.clear();
            return _plain_units_out;
        }

        auto lock = _mtx_revision.getLock();
        if (!canImport()) {
            _plain_units_out.clear();
            return _plain_units_out;
        }

        int numUnits = 0;
        int numLits = 0;
        std::vector<int> buf = _pcb.exportBuffer(-1, numUnits, numLits, AdaptiveClauseStore::UNITS, /*sortClauses=*/false);
        assert(numUnits >= numLits);

        _plain_units_out = std::vector<int>(buf.data()+(buf.size()-numUnits), buf.data()+buf.size());
        assert(_plain_units_out.size() == numUnits);
        for (int i = 0; i < _plain_units_out.size(); i++) assert(_plain_units_out[i] != 0);
        _stats.receivedClausesDigested += numUnits;
        _stats.histDigested->increase(1, numUnits);
        return _plain_units_out;
    }

    Mallob::Clause& get(AdaptiveClauseStore::ExportMode mode) override {

        if (_pcb.getCurrentlyUsedLiterals() == 0) {
            _clause_out.begin = nullptr;
            return _clause_out;
        }

        auto lock = _mtx_revision.getLock();
        if (!canImport()) {
            _clause_out.begin = nullptr;
            return _clause_out;
        }

        _clause_out.begin = _clause_out_data.data();
        if (_pcb.popClauseWeak(mode, _clause_out)) {
            _stats.receivedClausesDigested++;
            _stats.histDigested->increment(_clause_out.size);
            assert(_clause_out.size > 0);
            assert(_clause_out.lbd > 0);
            //assert(_clause_out.begin[0] != 0);
        } else {
            _clause_out.begin = nullptr;
        }

        return _clause_out;
    }

    size_t size() const override {
        int litsInUse = _pcb.getCurrentlyUsedLiterals();
        assert(litsInUse >= 0);
        return litsInUse;
    }

    ~AdaptiveImportManager() {}
};
