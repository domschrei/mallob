
#pragma once

#include <vector>
#include <list>

#include "app/sat/sharing/buffer/buffer_reader.hpp"
#include "buffer/priority_clause_buffer.hpp"
#include "../execution/solver_setup.hpp"
#include "util/logger.hpp"

class ImportBuffer {

private:
    SolverStatistics& _stats;
    PriorityClauseBuffer _pcb;
    int _max_clause_length;
    bool _reset_lbd;

    std::vector<int> _plain_units_out;
    std::vector<int> _clause_out_data;
    Mallob::Clause _clause_out;

public:
    ImportBuffer(const SolverSetup& setup, SolverStatistics& stats) : _stats(stats), 
        _pcb([&]() {
            PriorityClauseBuffer::Setup pcbSetup;
            pcbSetup.maxClauseLength = setup.strictClauseLengthLimit;
            pcbSetup.maxLbdPartitionedSize = 2;
            pcbSetup.numLiterals = setup.clauseBaseBufferSize * std::max(
                setup.minNumChunksPerSolver, 
                (int) (
                    ((float) setup.numBufferedClsGenerations) * 
                    setup.anticipatedLitsToImportPerCycle / setup.clauseBaseBufferSize
                )
            );
            pcbSetup.slotsForSumOfLengthAndLbd = false;
            pcbSetup.useChecksums = false;
            return pcbSetup;
        }()), _max_clause_length(setup.strictClauseLengthLimit),
        _reset_lbd(setup.resetLbdBeforeImport),
        _clause_out_data(setup.strictClauseLengthLimit),
        _clause_out(_clause_out_data.data(), 0, 0) {}

    void performImport(BufferReader& reader) {
        LOG(V2_INFO, "DBG perform import of size %i\n", reader.getRemainingSize());
        _pcb.addClauses(reader, nullptr);
        LOG(V2_INFO, "DBG %lu lits in import buffer\n", size());
    }

    void add(const Mallob::Clause& c) {
        if (ClauseMetadata::enabled()) {
            // Perform safety checks
            assert(c.size >= 3);
            // Heavy safety checks with false positives!
            /*
            unsigned long id; memcpy(&id, c.begin, sizeof(unsigned long));
            assert(id < std::numeric_limits<unsigned long>::max()/2
                    || log_return_false("Clause ID \"%lu\" found, which could be an error\n", id));
            for (size_t i = MALLOB_CLAUSE_METADATA_SIZE; i < c.size; i++)
                assert(std::abs(c.begin[i]) < 10000000 
                    || log_return_false("Literal \"%i\" found, error thrown for safety - "
                    "delete this assertion if your formula has >=10'000'000 variables\n", 
                    c.begin[i]));
            */
        }
        bool success = _pcb.addClause(c);
        if (!success) _stats.receivedClausesDropped++;
    }

    const std::vector<int>& getUnitsBuffer() {

        if (_pcb.getNumLiterals(1, 1) == 0) {
            _plain_units_out.clear();
            return _plain_units_out;
        }

        int numUnits = 0;
        std::vector<int> buf;
        buf = _pcb.exportBuffer(-1, numUnits, PriorityClauseBuffer::UNITS, /*sortClauses=*/false);

        _plain_units_out = std::vector<int>(buf.data()+(buf.size()-numUnits), buf.data()+buf.size());
        assert(_plain_units_out.size() == numUnits);
        for (int i = 0; i < _plain_units_out.size(); i++) assert(_plain_units_out[i] != 0);
        _stats.receivedClausesDigested += numUnits;
        _stats.histDigested->increase(1, numUnits);
        return _plain_units_out;
    }

    Mallob::Clause& get(PriorityClauseBuffer::ExportMode mode) {

        if (_pcb.getCurrentlyUsedLiterals() == 0) {
            _clause_out.begin = nullptr;
            return _clause_out;
        }

        _clause_out.begin = _clause_out_data.data();
        if (_pcb.popClauseWeak(mode, _clause_out)) {
            _stats.receivedClausesDigested++;
            _stats.histDigested->increment(_clause_out.size);
            if (_reset_lbd) _clause_out.lbd = _clause_out.size;
            assert(_clause_out.size > 0);
            assert(_clause_out.lbd > 0);
            //assert(_clause_out.begin[0] != 0);
        } else {
            _clause_out.begin = nullptr;
        }

        return _clause_out;
    }

    bool empty() const {
        return size() == 0;
    }

    size_t size() const {
        int litsInUse = _pcb.getCurrentlyUsedLiterals();
        assert(litsInUse >= 0);
        return litsInUse;
    }

    ~ImportBuffer() {
        if (_clause_out.begin != nullptr) free(_clause_out.begin);
    }
};
