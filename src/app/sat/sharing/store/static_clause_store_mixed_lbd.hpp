
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

#define BUCKET_SIZE 1000

class StaticClauseStoreMixedLbd : public GenericClauseStore {

private:
    struct Bucket {
        int data[BUCKET_SIZE];
        unsigned int size {0};
        int clauseLength {0};
    };
	Mutex addClauseLock;

	// Structures for EXPORTING
	std::vector<Bucket*> buckets;

public:
    StaticClauseStoreMixedLbd(int maxClauseLength, bool resetLbdAtExport) :
        GenericClauseStore(maxClauseLength, resetLbdAtExport) {}

    bool addClause(const Mallob::Clause& clause) override {
        if (!addClauseLock.tryLock()) return false;

        int bucketIdx = getBufferIdx(clause.size);

        while (bucketIdx >= buckets.size()) {
            Bucket* b = new Bucket();
            buckets.push_back(b);
        }
        Bucket* b = buckets[bucketIdx];
        b->clauseLength = clause.size;
        unsigned int top = b->size;
        if (top + clause.size+1 <= BUCKET_SIZE) {
            // copy the clause
            for (unsigned int i = 0; i < clause.size; i++) {
                b->data[top + i] = clause.begin[i];
            }
            b->data[top + clause.size] = clause.lbd; // lbd last
            // update bucket size
            b->size += clause.size+1;
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

    std::vector<int> exportBuffer(int limit, int& nbExportedClauses, ExportMode mode = ANY, bool sortClauses = true,
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
            
            if (nbRemainingLits < clause.size) break;
            if (clause.size == 0) continue;
            if (clause.size == 1 && mode == NONUNITS) continue;
            if (clause.size > 1 && mode == UNITS) break;

            while (b->size > 0 && nbRemainingLits >= clause.size) {
                assert(b->size - clause.size >= 0);
                clause.begin = b->data + b->size - (clause.size+1);
                clause.lbd = b->data[b->size - 1];
                assert(clause.lbd >= std::min(2, clause.size) && clause.lbd <= clause.size);
                if (_reset_lbd_at_export) clause.lbd = clause.size;
                clauseDataConverter(clause.begin);
                clauses.push_back(clause);
                nbRemainingLits -= clause.size;
                b->size -= clause.size+1;
            }
            if (clauses.empty()) continue;

            if (nbRemainingLits < clause.size) break;
        }

        std::sort(clauses.begin(), clauses.end());
        Mallob::Clause lastClause;
        for (auto& c : clauses) {
            assert(lastClause.begin == nullptr || lastClause == c || lastClause < c
                || log_return_false("[ERROR] %s > %s\n", lastClause.toStr().c_str(), c.toStr().c_str()));
            lastClause = c;
            bool success = builder.append(c);
            assert(success);
        }

        addClauseLock.unlock();
        nbExportedClauses = builder.getNumAddedClauses();
        return builder.extractBuffer();
    }

    BufferReader getBufferReader(int* data, size_t buflen, bool useChecksums = false) const override {
        return BufferReader(data, buflen, _max_clause_length, false, useChecksums);
    }

    ~StaticClauseStoreMixedLbd() {
        for (unsigned int i = 0; i < buckets.size(); i++) {
            delete buckets[i];
        }
    }

private:
    int getBufferIdx(int clauseLength) {
        return clauseLength-1;
    }
};
