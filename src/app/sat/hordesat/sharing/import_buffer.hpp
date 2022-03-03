
#ifndef DOMPASCH_MALLOB_IMPORT_BUFFER_HPP
#define DOMPASCH_MALLOB_IMPORT_BUFFER_HPP

#include <vector>
#include <list>

#include "adaptive_clause_database.hpp"
#include "app/sat/hordesat/solvers/solver_setup.hpp"

class ImportBuffer {

private:
    SolvingStatistics& _stats;
    AdaptiveClauseDatabase _cdb;

    std::vector<int> _ready_to_import_buffer;
	std::unique_ptr<BufferReader> _ready_to_import_reader;

    std::vector<int> _plain_units_out;
    Mallob::Clause _clause_out;

public:
    ImportBuffer(const SolverSetup& setup, SolvingStatistics& stats) : _stats(stats), 
        _cdb([&]() {
            AdaptiveClauseDatabase::Setup cdbSetup;
            cdbSetup.chunkSize = setup.clauseBaseBufferSize;
            cdbSetup.maxClauseLength = setup.strictClauseLengthLimit;
            cdbSetup.maxLbdPartitionedSize = 2;
            cdbSetup.numChunks = std::max(
                setup.minNumChunksPerSolver, 
                (int) (
                    ((float) setup.numBufferedClsGenerations) * 
                    setup.anticipatedLitsToImportPerCycle / setup.clauseBaseBufferSize
                )
            );
            cdbSetup.numProducers = 1;
            cdbSetup.slotsForSumOfLengthAndLbd = false;
            cdbSetup.useChecksums = false;
            return cdbSetup;
        }()) {

        _ready_to_import_reader.reset(new BufferReader());
    }

    void add(const Mallob::Clause& c) {
        _stats.receivedClauses++;
        bool success = _cdb.addClause(0, c);
        if (success) {
            _stats.receivedClausesInserted++;
        } else {
            _stats.discardedClauses++;
        }
    }

    int bulkAdd(const std::vector<Mallob::Clause>& clauses, std::function<bool(const Mallob::Clause&)> conditional) {
        _stats.receivedClauses += clauses.size();
        int admitted = _cdb.bulkAddClauses(0, clauses, conditional);
        _stats.receivedClausesInserted += admitted;
        _stats.receivedClausesFiltered += clauses.size()-admitted;
        return admitted;
    }

    std::vector<int> getUnitsBuffer() {
        int numUnits = 0;
        auto buf = _cdb.exportBuffer(-1, numUnits, AdaptiveClauseDatabase::UNITS, /*sortClauses=*/false);
        _plain_units_out = std::vector<int>(buf.data()+(buf.size()-numUnits), buf.data()+buf.size());
        assert(_plain_units_out.size() == numUnits);
        for (int i = 0; i < _plain_units_out.size(); i++) assert(_plain_units_out[i] != 0);
        _stats.digestedClauses += numUnits;
        _stats.histDigested->increase(1, numUnits);
        return _plain_units_out;
    }

    Mallob::Clause& get(AdaptiveClauseDatabase::ExportMode mode) {

        _clause_out = _ready_to_import_reader->getNextIncomingClause();
        if (_clause_out.begin != nullptr) {
            _stats.digestedClauses++;
            _stats.histDigested->increment(_clause_out.size);
            return _clause_out;
        }

        // Refill buffer with clauses from import database
        _ready_to_import_reader->releaseBuffer();
        int numClauses = 0;
        _ready_to_import_buffer = _cdb.exportBuffer(-1, numClauses, mode, /*sortClauses=*/false);
        if (numClauses == 0) return _clause_out;
        
        _ready_to_import_reader.reset(new BufferReader(_cdb.getBufferReader(_ready_to_import_buffer.data(), _ready_to_import_buffer.size())));
        _clause_out = _ready_to_import_reader->getNextIncomingClause();
        if (_clause_out.begin != nullptr) {
            _stats.digestedClauses++;
            _stats.histDigested->increment(_clause_out.size);
        }
        return _clause_out;
    }

};


#endif
