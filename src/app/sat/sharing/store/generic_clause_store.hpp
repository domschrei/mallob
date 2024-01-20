
#pragma once

#include <vector>

#include "app/sat/data/clause.hpp"
#include "app/sat/data/clause_histogram.hpp"
#include "app/sat/sharing/buffer/buffer_reader.hpp"

#define MALLOB_CLAUSE_STORE_STATIC_BY_LENGTH_MIXED_LBD -1
#define MALLOB_CLAUSE_STORE_STATIC_BY_LENGTH 0
#define MALLOB_CLAUSE_STORE_STATIC_BY_LBD 1
#define MALLOB_CLAUSE_STORE_ADAPTIVE 2
#define MALLOB_CLAUSE_STORE_ADAPTIVE_SIMPLE 3

class GenericClauseStore {

protected:
    int _max_eff_clause_length;
    bool _reset_lbd_at_export;
    ClauseHistogram _hist_deleted_in_slots;

public:
    GenericClauseStore(int maxEffClauseLength, bool resetLbdAtExport) :
        _max_eff_clause_length(maxEffClauseLength), _reset_lbd_at_export(resetLbdAtExport),
        _hist_deleted_in_slots(maxEffClauseLength) {}
    virtual ~GenericClauseStore() {}

    virtual bool addClause(const Mallob::Clause& c) = 0;
    virtual void addClauses(BufferReader& inputReader, ClauseHistogram* hist) = 0;

    enum ExportMode {UNITS, NONUNITS, ANY};
    virtual std::vector<int> exportBuffer(int size, int& nbExportedClauses, int& nbExportedLits,
        ExportMode mode = ANY, bool sortClauses = true,
        std::function<void(int*)> clauseDataConverter = [](int*){}) = 0;

    virtual BufferReader getBufferReader(int* data, size_t buflen, bool useChecksums = false) const = 0;
    virtual int getMaxAdmissibleEffectiveClauseLength() const {return _max_eff_clause_length;}
    virtual void setClauseDeletionCallback(int clauseLength, std::function<void(Mallob::Clause&)> cb) {}

    virtual int getCurrentlyUsedLiterals() const {return 0;}
    virtual std::string getCurrentlyUsedLiteralsReport() const {return std::string();}

    ClauseHistogram& getDeletedClausesHistogram() {
        return _hist_deleted_in_slots;
    }
};
