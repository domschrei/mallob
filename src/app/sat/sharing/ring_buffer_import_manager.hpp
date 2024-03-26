
#pragma once

#include <vector>

#include "app/sat/sharing/buffer/buffer_reader.hpp"
#include "app/sat/sharing/generic_import_manager.hpp"
#include "store/adaptive_clause_store.hpp"
#include "util/ringbuffer.hpp"

class RingBufferImportManager : public GenericImportManager {

private:
    RingBuffer _units;
    RingBuffer _clauses;

    std::vector<int> _plain_units_out;
    std::vector<int> _plain_clauses_out;
    size_t _plain_clauses_position;
    Mallob::Clause _clause_out;

public:
    RingBufferImportManager(const SolverSetup& setup, SolverStatistics& stats) :
        GenericImportManager(setup, stats),
		_units(getLiteralBudget(setup)), _clauses(1.5*getLiteralBudget(setup)) {}

    void performImport(BufferReader& reader) override {
        Mallob::Clause c = reader.getNextIncomingClause();
        while (c.begin != nullptr) {
            addSingleClause(c);
            c = reader.getNextIncomingClause();
        }
    }

    void addSingleClause(const Mallob::Clause& c) override {
        bool success;
        if (c.size == 1) {
            success = _units.produce(c.begin, 1, false);
        } else {
            int lbd = c.lbd;
            success = _clauses.produceInTwoChunks(&lbd, 1, c.begin, c.size, true);
        }
        if (!success) _stats.receivedClausesDropped++;
    }

    const std::vector<int>& getUnitsBuffer() override {

        auto lock = _mtx_revision.getLock();
        if (!canImport()) {
            _plain_units_out.clear();
            return _plain_units_out;
        }

        bool success = _units.consume(_plain_units_out);
        if (!success) {
            _plain_units_out.clear();
            return _plain_units_out;
        }

        _stats.receivedClausesDigested += _plain_units_out.size();
        _stats.histDigested->increase(1, _plain_units_out.size());
        return _plain_units_out;
    }

    Mallob::Clause& get(AdaptiveClauseStore::ExportMode mode) override {

        auto lock = _mtx_revision.getLock();
        if (!canImport()) {
            _clause_out.begin = nullptr;
            return _clause_out;
        }

        // Retrieve clauses from parallel ringbuffer as necessary
        if (_plain_clauses_out.empty() || _plain_clauses_position >= _plain_clauses_out.size()) {
            bool success = _clauses.consume(_plain_clauses_out);
            if (!success) _plain_clauses_out.clear();
            _plain_clauses_position = 0;
        }
        
        // Is there a clause?
        if (_plain_clauses_out.empty() || _plain_clauses_position >= _plain_clauses_out.size()) {
            _clause_out.begin = nullptr;
            return _clause_out;
        }

        // Extract a clause
        size_t start = _plain_clauses_position;
        size_t end = start;
        while (_plain_clauses_out[end] != 0) ++end;
        assert(_plain_clauses_out[end] == 0);
        assert(end-start >= 3);

        // Set glue
        _clause_out.lbd = _plain_clauses_out[start];
        assert(_clause_out.lbd > 0);
        // Make "clause" point to buffer
        _clause_out.begin = _plain_clauses_out.data() + start + 1;
        _clause_out.size = (end-start) - 1;

        _plain_clauses_position = end+1;
        _stats.receivedClausesDigested++;

        return _clause_out;
    }

    size_t size() const override {
        return -1;
    }
};
