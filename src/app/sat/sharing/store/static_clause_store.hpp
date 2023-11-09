
#pragma once

#include <cstdint>
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
	std::vector<std::pair<int, Bucket*>> buckets;

    const int _bucket_size;
    const bool _expand_buckets;
    const int _total_capacity;

    int _max_admissible_bucket_idx {INT32_MAX};
    int _max_admissible_bucket_lits;

public:
    StaticClauseStore(int maxClauseLength, bool resetLbdAtExport, int bucketSize, bool expandBuckets, int totalCapacity) :
        GenericClauseStore(maxClauseLength, resetLbdAtExport), _bucket_size(bucketSize),
        _expand_buckets(expandBuckets), _total_capacity(totalCapacity) {}

    bool addClause(const Mallob::Clause& clause) override {
        if (!addClauseLock.tryLock()) return false;

        int bucketIdx = getBufferIdx(clause.size, clause.lbd);

        if (bucketIdx >= buckets.size()) {
            buckets.resize(bucketIdx+1);
        }
        auto& [touchCount, b] = buckets[bucketIdx];
        if (bucketIdx > _max_admissible_bucket_idx) {
            touchCount++;
            addClauseLock.unlock();
            return false; // bucket too bad - reject clause
        }
        if (!b) {
            b = new Bucket(_bucket_size);
            b->clauseLength = clause.size;
            b->lbd = clause.lbd;
        }
        unsigned int top = b->size;
        while (top + clause.size > b->capacity()) {
            // do we expand buckets?
            if (!_expand_buckets) {
                touchCount++;
                addClauseLock.unlock();
                return false; // no
            }
            // should this bucket be expanded?
            if (bucketIdx == _max_admissible_bucket_idx && top + clause.size > _max_admissible_bucket_lits) {
                touchCount++;
                addClauseLock.unlock();
                return false; // no - fringe bucket, already too full
            }
            // expand bucket
            b->expand();
        }
        // copy the clause
        for (unsigned int i = 0; i < clause.size; i++) {
            b->data[top + i] = clause.begin[i];
        }
        // update bucket size
        b->size += clause.size;
        addClauseLock.unlock();
        return true;
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

        // Reset threshold for admissible clauses
        _max_admissible_bucket_idx = INT32_MAX;

        int nbRemainingLits = limit;
        std::vector<Mallob::Clause> clauses;
        bool cleaningUp {false};
        int nbRemainingAdmissibleLits = _total_capacity;
        int maxNonemptyBufferIdx = -1;
        size_t totalSizeBefore = 0;
        size_t totalSizeAfter = 0;

        // Traverse buckets by priority in descending order
        for (int i = 0; i < buckets.size(); i++) {
            auto& [touchCount, b] = buckets[i];
            if (!b) continue;

            Mallob::Clause clause;
            clause.size = b->clauseLength;
            clause.lbd = _reset_lbd_at_export ? clause.size : b->lbd;

            // Check admissible literals bounds?
            if (_expand_buckets) {
                if (!cleaningUp) {
                    int effectiveSize = b->size + b->clauseLength * touchCount;
                    if (nbRemainingAdmissibleLits < effectiveSize) {
                        // all admissible literals counted: update threshold,
                        LOG(V4_VVER, "[clausestore] new threshold @ idx %s:%i\n",
                            bufferIdxToStr(i).c_str(), nbRemainingAdmissibleLits);
                        _max_admissible_bucket_idx = i;
                        _max_admissible_bucket_lits = nbRemainingAdmissibleLits;
                        b->size = std::min((int)b->size, (nbRemainingAdmissibleLits / b->clauseLength) * b->clauseLength);
                        nbRemainingAdmissibleLits = 0;
                        // begin cleaning up all subsequent data
                        cleaningUp = true;
                    } else if (clause.size > 1) { // skip unit clauses
                        // subtract admissible literals from this bucket
                        nbRemainingAdmissibleLits -= effectiveSize;
                    }
                }
                totalSizeBefore += b->capacity();
                b->shrinkToFit();
                touchCount = 0;
                totalSizeAfter += b->capacity();
            }

            assert(clause.size > 0 && clause.lbd > 0);
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

            if (cleaningUp && i > _max_admissible_bucket_idx) {
                // all subsequent buckets are cleared completely
                if (b->size == 0) {
                    totalSizeAfter -= b->capacity();
                    delete b;
                    buckets[i].second = nullptr;
                }
            }

            if (buckets[i].second) maxNonemptyBufferIdx = i;
        }

        // reduce buckets vector to its required size
        if (cleaningUp) {
            buckets.resize(maxNonemptyBufferIdx+1);
            // some logging
            LOG(V4_VVER, "[clausestore] total size %lu->%lu - cap. %i - threshold @ idx %s\n",
                totalSizeBefore, totalSizeAfter, _total_capacity,
                bufferIdxToStr(_max_admissible_bucket_idx).c_str());
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
            if (buckets[i].second) delete buckets[i].second;
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

    std::string bufferIdxToStr(int bufferIdx) {
        std::string out = std::to_string(bufferIdx);
        if (bufferIdx < buckets.size() && buckets[bufferIdx].second) {
            auto b = buckets[bufferIdx].second;
            out += " (" + std::to_string(b->clauseLength) + "," + std::to_string(b->lbd) + ")";
        }
        return out;
    }
};
