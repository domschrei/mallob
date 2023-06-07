
#pragma once

#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <vector>

#include "app/sat/data/clause.hpp"
#include "app/sat/sharing/buffer/buffer_builder.hpp"
#include "app/sat/sharing/buffer/buffer_reader.hpp"
#include "app/sat/sharing/store/generic_clause_store.hpp"
#include "util/sys/threading.hpp"
#include "util/logger.hpp"
#include "static_clause_store_commons.hpp"

class StaticClauseStore : public GenericClauseStore {

private:
	Mutex addClauseLock;

	// Structures for EXPORTING
	std::vector<Bucket*> buckets;

    const int _bucket_size;

public:
    StaticClauseStore(int maxClauseLength, bool resetLbdAtExport, int bucketSize) :
        GenericClauseStore(maxClauseLength, resetLbdAtExport), _bucket_size(bucketSize) {}

    bool addClause(const Mallob::Clause& clause) override {
        if (!addClauseLock.tryLock()) return false;

        int bucketIdx = getBufferIdx(clause.size, clause.lbd);

        while (bucketIdx >= buckets.size()) {
            Bucket* b = new Bucket(_bucket_size);
            buckets.push_back(b);
        }
        Bucket* b = buckets[bucketIdx];
        b->clauseLength = clause.size;
        b->lbd = clause.lbd;
        unsigned int top = b->size;
        if (top + clause.size <= _bucket_size) {
            // copy the clause
            for (unsigned int i = 0; i < clause.size; i++) {
                b->data[top + i] = clause.begin[i];
            }
            // update bucket size
            b->size += clause.size;
            addClauseLock.unlock();
            return true;
        } else {
            addClauseLock.unlock();
            return false;
        }
    }

    void addClauses(BufferReader& inputReader, ClauseHistogram* hist) override {
        Mallob::Clause c = inputReader.getNextIncomingClause();
        while (c.begin != nullptr) {
            if (addClause(c) && hist) hist->increment(c.size);
            c = inputReader.getNextIncomingClause();
        }
    }

    std::vector<int> exportBuffer(int limit, int& nbExportedClauses, int& nbExportedLits,
            ExportMode mode = ANY, bool sortClauses = true,
            std::function<void(int*)> clauseDataConverter = [](int*){}) override {

        BufferBuilder builder(limit, _max_clause_length, false);

        // lock clause adding
        addClauseLock.lock();

        // Read clauses bucket by bucket (most important clauses first)
        int nbRemainingLits = limit;
        std::vector<Mallob::Clause> clauses;
        for (Bucket* b : buckets) {

            Mallob::Clause clause;
            clause.size = b->clauseLength;
            clause.lbd = _reset_lbd_at_export ? clause.size : b->lbd;

            if (nbRemainingLits < clause.size) break;
            if (clause.size == 0 || clause.lbd == 0) continue;
            if (clause.size == 1 && mode == NONUNITS) continue;
            if (clause.size > 1 && mode == UNITS) break;

            while (b->size > 0 && nbRemainingLits >= clause.size) {
                assert(b->size - clause.size >= 0);
                clause.begin = b->data + b->size - clause.size;
                clauseDataConverter(clause.begin);
                clauses.push_back(clause);
                nbRemainingLits -= clause.size;
                b->size -= clause.size;
            }
            if (clauses.empty()) continue;

            if (nbRemainingLits < clause.size) break;
        }

        nbExportedLits = 0;
        std::sort(clauses.begin(), clauses.end());
        Mallob::Clause lastClause;
        for (auto& c : clauses) {
            assert(lastClause.begin == nullptr || lastClause == c || lastClause < c
                || log_return_false("[ERROR] %s > %s\n", lastClause.toStr().c_str(), c.toStr().c_str()));
            lastClause = c;
            bool success = builder.append(c);
            assert(success);
            nbExportedLits += c.size;
        }

        addClauseLock.unlock();
        nbExportedClauses = builder.getNumAddedClauses();
        return builder.extractBuffer();
    }

    BufferReader getBufferReader(int* data, size_t buflen, bool useChecksums = false) const override {
        return BufferReader(data, buflen, _max_clause_length, false, useChecksums);
    }

    ~StaticClauseStore() {
        for (unsigned int i = 0; i < buckets.size(); i++) {
            delete buckets[i];
        }
    }

private:
    // (1,1) -> 0
    // (2,2) -> 1
    // (3,2) -> 2       3: 2 prev. buckets
    // (3,3) -> 3
    // (4,2) -> 4       4: 4 prev. buckets
    // (4,3) -> 5
    // (4,4) -> 6
    // (5,2) -> 7       5: 7 prev. buckets
    // (5,3) -> 8
    // (5,4) -> 9
    // (5,5) -> 10
    // (6,2) -> 11      6: 11 prev. buckets
    int getBufferIdx(int clauseLength, int lbd) {
        if (clauseLength <= 2) return clauseLength-1;
        int prevBuckets = ((clauseLength-2) * (clauseLength-1)) / 2 + 1;
        return prevBuckets + (lbd-2);
    }
};
