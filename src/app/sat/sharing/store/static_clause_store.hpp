
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

    // Whether buckets are expanded and shrunk dynamically (true)
    // or remain of a fixed, static size (false).
    const bool _expand_buckets;

    // Size of each bucket (in literals) if buckets are not expanded.
    const int _bucket_size;

    // If buckets are being expanded, this is the user-provided
    // limit on the global buffer size (in literals), disregarding
    // unit clauses.
    const int _total_capacity;

    // Current threshold bucket for storing new clauses.
    int _max_admissible_bucket_idx {INT32_MAX};
    // Max. admissible number of literals in the threshold bucket. 
    int _max_admissible_bucket_lits;

public:
    StaticClauseStore(int maxClauseLength, bool resetLbdAtExport, int bucketSize, bool expandBuckets, int totalCapacity) :
        GenericClauseStore(maxClauseLength, resetLbdAtExport), _expand_buckets(expandBuckets),
            _bucket_size(bucketSize), _total_capacity(totalCapacity) {}

    bool addClause(const Mallob::Clause& clause) override {
        if (!addClauseLock.tryLock()) return false;

        int bucketIdx = getBufferIdx(clause.size, clause.lbd);

        // Create bucket as necessary
        if (bucketIdx >= buckets.size()) {
            buckets.resize(bucketIdx+1);
        }
        auto& [touchCount, b] = buckets[bucketIdx];
        if (!b) {
            b = new Bucket(_bucket_size);
            b->clauseLength = clause.size;
            b->lbd = clause.lbd;
        }

        // Is the bucket "too bad" w.r.t. the current threshold?
        if (bucketIdx > _max_admissible_bucket_idx) {
            // bucket too bad - reject clause
            touchCount++;
            addClauseLock.unlock();
            return false;
        }

        // Is sufficient space available?
        unsigned int top = b->size;
        while (top + clause.size > b->capacity()) {
            // no -- do we expand buckets?
            if (!_expand_buckets) {
                touchCount++;
                addClauseLock.unlock();
                return false; // -- no
            }
            // should this bucket be expanded?
            if (bucketIdx == _max_admissible_bucket_idx && top + clause.size > _max_admissible_bucket_lits) {
                // -- no - fringe bucket, already too full
                touchCount++;
                addClauseLock.unlock();
                return false;
            }
            // -- yes, expand bucket (2x)
            b->expand();
        }

        // write the clause into the buffer
        for (unsigned int i = 0; i < clause.size; i++) {
            b->data[top + i] = clause.begin[i];
        }

        // update bucket size
        b->size += clause.size;

        // success
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

        // Builder object for our output
        BufferBuilder builder(limit, _max_clause_length, false);

        // lock clause adding
        addClauseLock.lock();

        // Reset threshold for admissible clauses
        _max_admissible_bucket_idx = INT32_MAX;

        // Fields for iterating over buckets
        int nbRemainingLits = limit; // # lits to export
        std::vector<Mallob::Clause> clauses; // exported clauses
        int nbRemainingAdmissibleLits = _total_capacity; // # lits until cleanup
        bool cleaningUp {false}; // in the process of cleaning buckets?
        int maxNonemptyBufferIdx = -1; // last seen non-empty bucket
        size_t totalSizeBefore = 0; // total space occupied before sweep
        size_t totalSizeAfter = 0; // total space occupied after sweep

        // Sweep over buckets by priority in descending order
        for (int i = 0; i < buckets.size(); i++) {
            auto& [touchCount, b] = buckets[i];
            if (!b) continue; // bucket is not initialized

            // Initialize generic clause object for this bucket
            Mallob::Clause clause;
            clause.size = b->clauseLength;
            clause.lbd = _reset_lbd_at_export ? clause.size : b->lbd;

            // Check admissible literals bounds?
            if (_expand_buckets) {
                // Not yet in cleaning up stage
                if (!cleaningUp) {
                    // Check if this bucket, together with its "shadow insertions"
                    // tracked by the touchCount, exceed our capacity
                    int effectiveSize = b->size + b->clauseLength * touchCount;
                    if (nbRemainingAdmissibleLits < effectiveSize) {
                        // Capacity exceeded:
                        // update threshold,
                        _max_admissible_bucket_idx = i;
                        _max_admissible_bucket_lits = nbRemainingAdmissibleLits;
                        // trim buffer according to the remaining capacity,
                        b->size = std::min((int)b->size, (nbRemainingAdmissibleLits / b->clauseLength) * b->clauseLength);
                        // begin cleaning up all subsequent data
                        nbRemainingAdmissibleLits = 0;
                        cleaningUp = true;
                    } else if (clause.size > 1) {
                        // subtract admissible literals from this bucket
                        // except if the buffer stores unit clauses
                        nbRemainingAdmissibleLits -= effectiveSize;
                    }
                } else if (i > _max_admissible_bucket_idx) {
                    // clean up this bucket (see below)
                    b->size = 0;
                }
                // actual space clean-up takes place here:
                // bucket is shrunk to its current capacity
                totalSizeBefore += b->capacity();
                b->shrinkToFit();
                touchCount = 0;
                totalSizeAfter += b->capacity();
            }

            // follow traversal instruction
            assert(clause.size > 0 && clause.lbd > 0);
            if (clause.size == 1 && mode == NONUNITS) continue;
            if (clause.size > 1 && mode == UNITS) break;

            // write clauses into export buffer
            while (b->size > 0 && nbRemainingLits >= clause.size) {
                assert(b->size - clause.size >= 0);
                clause.begin = b->data + b->size - clause.size;
                clauseDataConverter(clause.begin);
                clauses.push_back(clause);
                nbRemainingLits -= clause.size;
                b->size -= clause.size;
            }

            // track most recent non-empty bucket
            if (buckets[i].second && buckets[i].second->size > 0)
                maxNonemptyBufferIdx = i;
        }

        if (cleaningUp) {
            // some logging on insertion threshold
            LOG(V4_VVER, "[clausestore] totalsize %lu->%lu - thresh B%s - maxne B%s\n",
                totalSizeBefore, totalSizeAfter,
                bufferIdxToStr(_max_admissible_bucket_idx).c_str(),
                bufferIdxToStr(maxNonemptyBufferIdx).c_str());
        }

        // Sort exported clauses and forward them to the builder
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
