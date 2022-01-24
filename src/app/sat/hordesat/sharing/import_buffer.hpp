
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
    std::list<Mallob::Clause> _deferred;

    std::vector<int> _ready_to_import_buffer;
	BufferReader _ready_to_import_reader;

    std::vector<int> _plain_units_out;
    Mallob::Clause _clause_out;

public:
    ImportBuffer(const SolverSetup& setup, SolvingStatistics& stats) : _stats(stats), 
        _cdb(
			setup.strictClauseLengthLimit,
            2, 
			setup.clauseBaseBufferSize, 
			std::max(
                setup.minNumChunksPerSolver, 
                (int) (
                    ((float) setup.numBufferedClsGenerations) * 
                    setup.anticipatedLitsToImportPerCycle / setup.clauseBaseBufferSize
                )
            ), 
            1
		) {}

    ~ImportBuffer() {
        for (auto& c : _deferred) {
            free(c.begin);
        }
    }

    void add(const Mallob::Clause& c) {
        _stats.receivedClauses++;
        auto result = _cdb.addClause(0, c);
        if (result == AdaptiveClauseDatabase::DROP) {
            _stats.discardedClauses++;
        } else if (result == AdaptiveClauseDatabase::TRY_LATER) {
            // defer clause
            _stats.deferredClauses++;
            _deferred.push_back(c.copy());
        } else {
            _stats.receivedClausesInserted++;
            // success: also try to insert clauses deferred earlier
            addDeferredClauses();
        }
    }

    void bulkAdd(const std::vector<Mallob::Clause>& clauses, std::function<bool(const Mallob::Clause&)> conditional) {
        _stats.receivedClauses += clauses.size();
        addDeferredClauses();
        //for (auto& c : clauses) add(c);
        _cdb.bulkAddClauses(0, clauses, _deferred, _stats, conditional);
    }

    std::vector<int> getUnitsBuffer() {
        int numUnits = 0;
        auto buf = _cdb.exportBuffer(-1, numUnits, 1, 1, /*sortClauses=*/false);
        _plain_units_out = std::vector<int>(buf.data()+(buf.size()-numUnits), buf.data()+buf.size());
        assert(_plain_units_out.size() == numUnits);
        for (int i = 0; i < _plain_units_out.size(); i++) assert(_plain_units_out[i] != 0);
        _stats.digestedClauses += numUnits;
        _stats.histDigested->increase(1, numUnits);
        return _plain_units_out;
    }

    enum GetMode {UNITS_ONLY, NONUNITS_ONLY, ANY};
    Mallob::Clause& get(GetMode mode) {

        _clause_out = _ready_to_import_reader.getNextIncomingClause();
        if (_clause_out.begin != nullptr) {
            _stats.digestedClauses++;
            _stats.histDigested->increment(_clause_out.size);
            return _clause_out;
        }

        // Refill buffer with clauses from import database

        int minLength = -1;
        int maxLength = -1;
        if (mode == UNITS_ONLY) {
            minLength = 1;
            maxLength = 1;
        } else if (mode == NONUNITS_ONLY) {
            minLength = 2;
        }

        _ready_to_import_reader.releaseBuffer();
        int numClauses = 0;
        _ready_to_import_buffer = _cdb.exportBuffer(-1, numClauses, minLength, maxLength, /*sortClauses=*/false);
        if (numClauses == 0) return _clause_out;
        
        _ready_to_import_reader = _cdb.getBufferReader(_ready_to_import_buffer.data(), _ready_to_import_buffer.size());
        _clause_out = _ready_to_import_reader.getNextIncomingClause();
        if (_clause_out.begin != nullptr) {
            _stats.digestedClauses++;
            _stats.histDigested->increment(_clause_out.size);
        }
        return _clause_out;
    }

private:
    void addDeferredClauses() {
        if (_deferred.empty()) return;
        std::vector<Mallob::Clause> copiedVec(_deferred.begin(), _deferred.end());
        _deferred.clear();
        LOG(V2_INFO, "bulk add deferred cls, size %i\n", copiedVec.size());
        _cdb.bulkAddClauses(0, copiedVec, _deferred, _stats);
        for (auto& cls : copiedVec) free(cls.begin);
    }

};


#endif
