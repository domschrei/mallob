
#pragma once

#include <climits>
#include <stdio.h>
#include <vector>

#include "app/sat/data/clause.hpp"
#include "app/sat/sharing/buffer/buffer_builder.hpp"
#include "app/sat/sharing/buffer/buffer_reader.hpp"
#include "app/sat/sharing/store/generic_clause_store.hpp"
#include "util/sys/threading.hpp"
#include "static_clause_store_commons.hpp"

class StaticClauseStoreByLbd : public GenericClauseStore {

private:
	Mutex addClauseLock;

	// Structures for EXPORTING
	std::vector<Bucket*> buckets;

    const int _bucket_size;

public:
    StaticClauseStoreByLbd(int maxEffClauseLength, bool resetLbdAtExport, int bucketSize) :
        GenericClauseStore(maxEffClauseLength, resetLbdAtExport), _bucket_size(bucketSize) {}

    bool addClause(const Mallob::Clause& clause) override {
        if (!addClauseLock.tryLock()) return false;

        int bucketIdx = clause.lbd-1;
        assert(bucketIdx >= 0);

        while (bucketIdx >= buckets.size()) {
            Bucket* b = new Bucket(_bucket_size);
            buckets.push_back(b);
        }
        Bucket* b = buckets[bucketIdx];
        b->lbd = clause.lbd;
        unsigned int top = b->size;
        if (top + clause.size + 1 <= _bucket_size) {
            // copy the clause (clause size at the end)
            for (unsigned int i = 0; i < clause.size; i++) {
                b->data[top+i] = clause.begin[i];
            }
            b->data[top+clause.size] = clause.size;
            // update bucket size
            b->size += clause.size + 1;
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

        BufferBuilder builder(limit, _max_eff_clause_length, false);

        // lock clause adding
        addClauseLock.lock();

        // Read clauses bucket by bucket (most important clauses first)
        std::vector<Mallob::Clause> clauses;
        int nbRemainingLits = limit;
        for (Bucket* b : buckets) {

            Mallob::Clause clause;
            clause.lbd = b->lbd;

            if (clause.lbd == 0) continue;

            while (b->size > 0) {
                clause.size = b->data[b->size-1];
                assert(clause.size > 0 && clause.size < 256);
                if (nbRemainingLits < clause.size) break;
                if (clause.size == 1 && mode == NONUNITS) continue;
                if (clause.size > 1 && mode == UNITS) break;
                assert(b->size - clause.size - 1 >= 0);
                if (_reset_lbd_at_export) clause.lbd = clause.size;
                clause.begin = b->data + b->size - clause.size - 1;
                clauseDataConverter(clause.begin);
                clauses.push_back(clause);
                nbRemainingLits -= clause.size;
                b->size -= clause.size + 1;
            }

            if (nbRemainingLits < clause.size) break;
        }

        nbExportedLits = 0;
        // Sort all flushed clauses by length -> lbd -> lexicographically
        std::sort(clauses.begin(), clauses.end());
        Mallob::Clause lastClause;
        for (auto& c : clauses) {
            assert(lastClause.begin == nullptr || lastClause == c || lastClause < c
                || log_return_false("[ERROR] %s > %s\n", lastClause.toStr().c_str(), c.toStr().c_str()));
            lastClause = c;
            bool success = builder.append(c);
            assert(success);
            nbExportedLits += c.size - ClauseMetadata::numInts();
        }

        addClauseLock.unlock();
        nbExportedClauses = builder.getNumAddedClauses();
        return builder.extractBuffer();
    }

    std::vector<int> readBuffer() override {

        BufferBuilder builder(INT_MAX, _max_eff_clause_length, false);

        // lock clause adding
        addClauseLock.lock();

        // Read clauses bucket by bucket (most important clauses first)
        std::vector<Mallob::Clause> clauses;
        for (Bucket* b : buckets) {

            Mallob::Clause clause;
            clause.lbd = b->lbd;
            if (clause.lbd == 0) continue;

            for (size_t j = b->size-1; j > 0; j -= clause.size+1) {
                clause.size = b->data[j];
                assert(clause.size > 0 && clause.size < 256);
                assert(j - clause.size >= 0);
                if (_reset_lbd_at_export) clause.lbd = clause.size;
                clause.begin = b->data + j - clause.size;
                clauses.push_back(clause);
            }
        }

        // Sort all flushed clauses by length -> lbd -> lexicographically
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
        return builder.extractBuffer();
    }

    BufferReader getBufferReader(int* data, size_t buflen, bool useChecksums = false) const override {
        return BufferReader(data, buflen, _max_eff_clause_length, false, useChecksums);
    }

    ~StaticClauseStoreByLbd() {
        for (unsigned int i = 0; i < buckets.size(); i++) {
            delete buckets[i];
        }
    }
};
