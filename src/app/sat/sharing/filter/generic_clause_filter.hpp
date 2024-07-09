
#pragma once

#include <stddef.h>
#include <atomic>
#include "app/sat/sharing/filter/produced_clause_filter_commons.hpp"
class GenericClauseStore;
class Logger;
namespace Mallob { struct Clause; }
struct ProducedClauseCandidate;

#define MALLOB_CLAUSE_FILTER_NONE 0
#define MALLOB_CLAUSE_FILTER_BLOOM 1
#define MALLOB_CLAUSE_FILTER_EXACT 2
#define MALLOB_CLAUSE_FILTER_EXACT_DISTRIBUTED 3

class GenericClauseFilter {

protected:
    std::atomic_int _epoch {0};
    GenericClauseStore& _clause_store;

public:
    GenericClauseFilter(GenericClauseStore& clauseStore) : _clause_store(clauseStore) {}
    virtual ~GenericClauseFilter() {}

    enum ExportResult {ADMITTED, FILTERED, DROPPED};
    virtual ExportResult tryRegisterAndInsert(ProducedClauseCandidate&& c, GenericClauseStore* storeOrNullptr = nullptr) = 0;
    virtual cls_producers_bitset confirmSharingAndGetProducers(Mallob::Clause& c, int epoch) = 0;
    virtual bool admitSharing(Mallob::Clause& c, int epoch) = 0;
    virtual size_t size(int clauseLength = 0) const = 0;

    virtual bool collectGarbage(const Logger& logger) {return false;}

    void updateEpoch(int epoch) {
        _epoch.store(epoch, std::memory_order_relaxed);
    }

    // Locking can be a no-op if the data structure does not require exclusive access.
    virtual bool tryAcquireLock(int clauseLength = 0) {return true;}
    virtual void acquireLock(int clauseLength = 0) {}
    virtual void releaseLock(int clauseLength = 0) {}
    virtual void acquireAllLocks() {}
    virtual void releaseAllLocks() {}
};
