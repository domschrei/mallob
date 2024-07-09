
#pragma once

#include <atomic>
#include <climits>
#include <cstdint>
#include <stdio.h>
#include <vector>

#include "app/sat/data/clause.hpp"
#include "app/sat/data/clause_metadata.hpp"
#include "app/sat/sharing/buffer/buffer_builder.hpp"
#include "app/sat/sharing/buffer/buffer_reader.hpp"
#include "app/sat/sharing/store/generic_clause_store.hpp"
#include "util/params.hpp"
#include "util/sys/threading.hpp"
#include "util/logger.hpp"
#include "static_clause_store_commons.hpp"

template <bool Concurrent>
class StaticClauseStore : public GenericClauseStore {

private:
	Mutex addClauseLock;

    // Structures for EXPORTING. Each bucket may be null (if never inserted to)
    // and is accompanied by a counter for "shadow insertions" which did not
    // actually take place but which we need to track for updating admission
    // thresholds.
    std::vector<std::pair<int, Bucket*>> buckets;

    // Whether buckets are expanded and shrunk dynamically (true)
    // or remain of a fixed, static size (false).
    const bool _expand_buckets;
    const int _free_clause_length_limit;

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
    std::atomic_int _current_max_eff_clause_length;

    std::vector<int> _bucket_idx_to_priority_idx;
    std::vector<int> _priority_idx_to_bucket_idx;


public:
    enum BucketPriorityMode {
        LENGTH_FIRST,
        LBD_FIRST,
    };
    // Helper struct to navigate clause store buckets according to a certain priority.
    // Features an *inner* stage, where all clauses of certain quality limits (length, LBD)
    // are traversed in a certain order, and an *outer* stage, where all remaining clauses
    // are traversed, again in a certain order.
    struct BucketIndex {
        const Parameters& params;
        int index {0};
        int length {1};
        int lbd {1};
        BucketPriorityMode modeInner;
        BucketPriorityMode modeOuter;
        int thresholdLength = -1;
        int thresholdLbd = -1;
        bool inner {true};
        BucketIndex(const Parameters& params, BucketPriorityMode modeInner, BucketPriorityMode modeOuter, int thresholdLength = -1, int thresholdLbd = -1) :
            params(params), modeInner(modeInner), modeOuter(modeOuter), thresholdLength(thresholdLength),
                thresholdLbd(std::min(thresholdLbd, thresholdLength)) {}

        int next() {
            if (inner && (lbd == thresholdLbd) && (length == thresholdLength)) {
                // Switch from inner to outer part
                inner = false;
                if (modeOuter == LENGTH_FIRST && lbd < length) {
                    // Proceed with all the short clauses of high LBD
                    // where we only considered low-LBD ones so far
                    lbd = thresholdLbd+1;
                    length = lbd;
                } else {
                    // Proceed with the next clause length
                    // since we already considered ALL shorter clauses
                    length++;
                    lbd = 2;
                }
            } else {
                // Normal handling of inner or outer part
                auto mode = inner ? modeInner : modeOuter;
                if (mode == LENGTH_FIRST) {
                    // Gone through all of the LBD values of the current length?
                    if (lbd == (inner ? std::min(thresholdLbd, std::min(length, params.strictLbdLimit()))
                            : std::min(length, params.strictLbdLimit()))) {
                        // Go to next length, lowest admissible LBD
                        length++;
                        lbd = (inner || length > thresholdLength) ? 2 : thresholdLbd+1;
                        assert(lbd <= length);
                    } else {
                        // Go to next LBD value for same length
                        lbd++;
                    }
                }
                if (mode == LBD_FIRST) {
                    // Gone through all clause lengths with the current LBD?
                    if (length == (inner ? thresholdLength : params.strictClauseLengthLimit()+ClauseMetadata::numInts())) {
                        // Go to next LBD, lowest length
                        lbd++;
                        length = (inner || lbd > thresholdLbd) ? lbd : thresholdLength+1;
                    } else {
                        // Go to next length for same LBD
                        length++;
                        lbd = std::max(lbd, 2); // if going from length 1 to 2 ...
                    }
                }
            }
            index = getBufferIdx(length, lbd);
            return index;
        }
    };

    StaticClauseStore(const Parameters& params, bool resetLbdAtExport, int bucketSize, bool expandBuckets, int totalCapacity) :
        GenericClauseStore(params.strictClauseLengthLimit()+ClauseMetadata::numInts(), resetLbdAtExport), _expand_buckets(expandBuckets),
            _free_clause_length_limit(params.freeClauseLengthLimit()), _bucket_size(bucketSize), _total_capacity(totalCapacity) {

        // Build bijection between (physical) bucket index and priority index
        // (i.e., in which order the buckets are being sweeped)
        BucketIndex index(params,
            params.lbdPriorityInner() ? LBD_FIRST : LENGTH_FIRST,
            params.lbdPriorityOuter() ? LBD_FIRST : LENGTH_FIRST,
            params.qualityClauseLengthLimit(), params.qualityLbdLimit());
        // Forward direction
        while (index.length <= _max_eff_clause_length
                && index.lbd <= params.strictLbdLimit()) {
            _priority_idx_to_bucket_idx.push_back(index.index);
            index.next();
        }
        // Backward direction
        _bucket_idx_to_priority_idx.reserve(_priority_idx_to_bucket_idx.size());
        for (int prioIdx = 0; prioIdx < _priority_idx_to_bucket_idx.size(); prioIdx++) {
            int bucketIdx = _priority_idx_to_bucket_idx[prioIdx];
            if (bucketIdx >= _bucket_idx_to_priority_idx.size())
                _bucket_idx_to_priority_idx.resize(bucketIdx+1);
            _bucket_idx_to_priority_idx[bucketIdx] = prioIdx;
        }
    }

    bool addClause(const Mallob::Clause& clause) override {
        if (Concurrent && !addClauseLock.tryLock()) return false;

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
        int priorityIdx = -1;
        if (_max_admissible_bucket_idx < INT32_MAX) {
            priorityIdx = getPriorityIndex(bucketIdx);
            if (priorityIdx > _max_admissible_bucket_idx) {
                // bucket too bad - reject clause
                touchCount++;
                if (Concurrent) addClauseLock.unlock();
                return false;
            }
        }

        // Is sufficient space available?
        unsigned int top = b->size;
        while (top + clause.size > b->capacity()) {
            // no -- do we expand buckets?
            if (!_expand_buckets) {
                touchCount++;
                if (Concurrent) addClauseLock.unlock();
                return false; // -- no
            }
            // should this bucket be expanded?
            if (priorityIdx == _max_admissible_bucket_idx && top + clause.size > _max_admissible_bucket_lits) {
                // -- no - fringe bucket, already too full
                touchCount++;
                if (Concurrent) addClauseLock.unlock();
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
        if (Concurrent) addClauseLock.unlock();
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
        BufferBuilder builder(limit, _max_eff_clause_length, false);
        builder.setFreeClauseLengthLimit(_free_clause_length_limit);

        // lock clause adding
        if (Concurrent) addClauseLock.lock();

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
        std::vector<std::pair<Bucket*, int>> shrinkableBucketsWithSize;

        // Sweep over buckets *by priority* in descending order
        for (int prioIdx = 0; prioIdx < _priority_idx_to_bucket_idx.size(); prioIdx++) {
            int i = getBucketIdx(prioIdx);
            if (i >= buckets.size()) continue; // bucket has never been created
            auto& [touchCount, b] = buckets[i];
            if (!b) continue; // bucket is not initialized
            //LOG(V4_VVER, "[clausestore] layout idx %i, priority idx %i, (%i,%i)\n", i, prioIdx, b->clauseLength, b->lbd);

            // Initialize generic clause object for this bucket
            Mallob::Clause clause;
            clause.size = b->clauseLength;
            clause.lbd = _reset_lbd_at_export ? clause.size : b->lbd;

            // Check admissible literals bounds?
            if (_expand_buckets && nbRemainingAdmissibleLits < INT32_MAX) {
                // Not yet in cleaning up stage
                if (!cleaningUp && clause.size-ClauseMetadata::numInts() > _free_clause_length_limit) {
                    // Check if this bucket, together with its "shadow insertions"
                    // tracked by the touchCount, exceed our capacity
                    int effectiveSize = b->size + b->clauseLength * touchCount;
                    if (nbRemainingAdmissibleLits < effectiveSize) {
                        // Capacity exceeded:
                        // update threshold,
                        _max_admissible_bucket_idx = prioIdx;
                        _max_admissible_bucket_lits = nbRemainingAdmissibleLits;
                        _current_max_eff_clause_length.store(b->clauseLength, std::memory_order_relaxed);
                        // trim buffer according to the remaining capacity,
                        b->size = std::min((int)b->size, (nbRemainingAdmissibleLits / b->clauseLength) * b->clauseLength);
                        // begin cleaning up all subsequent data
                        nbRemainingAdmissibleLits = 0;
                        cleaningUp = true;
                    } else {
                        // subtract admissible literals from this bucket
                        // except if the buffer stores "free" clauses
                        nbRemainingAdmissibleLits -= effectiveSize;
                    }
                } else if (cleaningUp && i > _max_admissible_bucket_idx) {
                    // clean up this bucket (see below)
                    b->size = 0;
                }
                // consider space clean-up of this bucket, reset shadow insertions
                totalSizeBefore += b->capacity();
                if (b->shrinkable()) shrinkableBucketsWithSize.push_back({b, b->size});
                else totalSizeAfter += b->capacity();
                touchCount = 0;
            }

            // follow traversal instruction
            assert(clause.size > 0 && clause.lbd > 0);
            if (clause.size-ClauseMetadata::numInts() == 1 && mode == NONUNITS) continue;
            if (clause.size-ClauseMetadata::numInts() > 1 && mode == UNITS) break;

            // write clauses into export buffer
            const int nbLitsInClause = clause.size-ClauseMetadata::numInts();
            while (b->size > 0 && (nbLitsInClause <= _free_clause_length_limit || nbRemainingLits >= nbLitsInClause)) {
                assert(b->size - clause.size >= 0);
                clause.begin = b->data + b->size - clause.size;
                clauseDataConverter(clause.begin);
                clauses.push_back(clause);
                if (nbLitsInClause > _free_clause_length_limit)
                    nbRemainingLits -= clause.size-ClauseMetadata::numInts();
                b->size -= clause.size;
            }

            // track most recent non-empty bucket
            if (buckets[i].second && buckets[i].second->size > 0)
                maxNonemptyBufferIdx = i;
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
            nbExportedLits += c.size - ClauseMetadata::numInts();
        }

        // Now that all clauses have been read, perform actual shrinkage of buckets
        for (auto& [b, size] : shrinkableBucketsWithSize) {
            b->shrinkToFit(size);
            totalSizeAfter += b->capacity();
        }

        if (cleaningUp) {
            // some logging on insertion threshold
            LOG(V5_DEBG, "[clausestore] totalsize %lu->%lu - thresh B%s - maxne B%s\n",
                totalSizeBefore, totalSizeAfter,
                bufferIdxToStr(_max_admissible_bucket_idx).c_str(),
                bufferIdxToStr(maxNonemptyBufferIdx).c_str());
        }

        if (Concurrent) addClauseLock.unlock();
        nbExportedClauses = builder.getNumAddedClauses();
        return builder.extractBuffer();
    }

    std::vector<int> readBuffer() override {

        // Builder object for our output
        BufferBuilder builder(INT_MAX, _max_eff_clause_length, false);
        builder.setFreeClauseLengthLimit(_free_clause_length_limit);

        // lock clause adding
        if (Concurrent) addClauseLock.lock();

        // Reset threshold for admissible clauses
        _max_admissible_bucket_idx = INT32_MAX;

        // Fields for iterating over buckets
        std::vector<Mallob::Clause> clauses; // exported clauses

        // Sweep over buckets *by priority* in descending order
        for (int prioIdx = 0; prioIdx < _priority_idx_to_bucket_idx.size(); prioIdx++) {
            int i = getBucketIdx(prioIdx);
            if (i >= buckets.size()) continue; // bucket has never been created
            auto& [touchCount, b] = buckets[i];
            if (!b) continue; // bucket is not initialized
            //LOG(V4_VVER, "[clausestore] layout idx %i, priority idx %i, (%i,%i)\n", i, prioIdx, b->clauseLength, b->lbd);

            // Initialize generic clause object for this bucket
            Mallob::Clause clause;
            clause.size = b->clauseLength;
            clause.lbd = _reset_lbd_at_export ? clause.size : b->lbd;
            assert(clause.size > 0 && clause.lbd > 0);

            // write clauses into export buffer
            for (size_t j = 0; j+clause.size <= b->size; j += clause.size) {
                clause.begin = b->data + j;
                clauses.push_back(clause);
            }
        }

        // Sort exported clauses and forward them to the builder
        std::sort(clauses.begin(), clauses.end());
        Mallob::Clause lastClause;
        for (auto& c : clauses) {
            assert(lastClause.begin == nullptr || lastClause == c || lastClause < c
                || log_return_false("[ERROR] %s > %s\n", lastClause.toStr().c_str(), c.toStr().c_str()));
            lastClause = c;
            bool success = builder.append(c);
            assert(success);
        }

        if (Concurrent) addClauseLock.unlock();
        return builder.extractBuffer();
    }

    BufferReader getBufferReader(int* data, size_t buflen, bool useChecksums = false) const override {
        return BufferReader(data, buflen, _max_eff_clause_length, false, useChecksums);
    }

    virtual int getMaxAdmissibleEffectiveClauseLength() const override {
        int priorityIdx = -1;
        if (_max_admissible_bucket_idx == INT32_MAX) return _max_eff_clause_length;
        return _current_max_eff_clause_length.load(std::memory_order_relaxed);
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
    static int getBufferIdx(int clauseLength, int lbd) {
        if (clauseLength <= 2) return clauseLength-1;
        int prevBuckets = ((clauseLength-2) * (clauseLength-1)) / 2 + 1;
        return prevBuckets + (lbd-2);
    }
    int getBucketIdx(int priorityIdx) {
        assert(priorityIdx >= 0 && priorityIdx < _priority_idx_to_bucket_idx.size());
        return _priority_idx_to_bucket_idx[priorityIdx];
    }
    int getPriorityIndex(int bucketIdx) {
        assert(bucketIdx >= 0 && bucketIdx < _bucket_idx_to_priority_idx.size());
        return _bucket_idx_to_priority_idx[bucketIdx];
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
