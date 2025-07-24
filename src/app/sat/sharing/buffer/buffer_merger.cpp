
#include <assert.h>
#include <string.h>
#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>

#include "app/sat/data/clause_metadata.hpp"
#include "app/sat/sharing/buffer/buffer_builder.hpp"
#include "app/sat/sharing/buffer/buffer_reader.hpp"
#include "app/sat/sharing/store/static_clause_store.hpp"
#include "buffer_merger.hpp"
#include "util/logger.hpp"
#include "util/random.hpp"
#include "util/tsl/robin_set.h"
#include "app/sat/sharing/buffer/buffer_iterator.hpp"
#include "app/sat/sharing/store/generic_clause_store.hpp"
#include "robin_hash.h"

BufferMerger::BufferMerger(int sizeLimit, int maxEffClauseLength, int maxFreeEffClauseLength, bool slotsForSumOfLengthAndLbd, bool useChecksum) :
    _size_limit(sizeLimit), _max_eff_clause_length(maxEffClauseLength), _max_free_eff_clause_length(maxFreeEffClauseLength),
    _slots_for_sum_of_length_and_lbd(slotsForSumOfLengthAndLbd), _use_checksum(useChecksum) {}

BufferMerger::BufferMerger(StaticClauseStore<false>* mergeStore, int sizeLimit, int maxEffClauseLength, bool slotsForSumOfLengthAndLbd, bool useChecksum) :
    _size_limit(sizeLimit), _max_eff_clause_length(maxEffClauseLength),
    _slots_for_sum_of_length_and_lbd(slotsForSumOfLengthAndLbd), _use_checksum(useChecksum), _merge_store(mergeStore) {}

void BufferMerger::add(BufferReader&& reader) {_readers.push_back(std::move(reader));}

std::vector<int> BufferMerger::mergeDiscardingExcess() {
    return merge(nullptr, nullptr);
}
std::vector<int> BufferMerger::mergePreservingExcess(std::vector<int>& excessOut) {
    return merge(&excessOut, nullptr);
}
std::vector<int> BufferMerger::mergePreservingExcessWithRandomTieBreaking(std::vector<int>& excessOut, SplitMix64Rng& rng) {
    return merge(&excessOut, &rng);
}

std::vector<int> BufferMerger::mergePriorityBased(const Parameters& params, std::vector<int>& excessOut, SplitMix64Rng& rng) {

    // Create a store holding all clauses from all producers
    assert(_merge_store);
    for (auto& reader : _readers) {
        while (true) {
            Mallob::Clause c = reader.getNextIncomingClause();
            if (!c.begin) break;
            _merge_store->addClause(c);
        }
    }

    // Flush store completely, which brings all clauses into the proper, priority-based order
    int nbExportedCls, nbExportedLits;
    std::vector<int> storeOutput = _merge_store->exportBuffer(INT32_MAX, nbExportedCls, nbExportedLits, GenericClauseStore::ANY, false);

    // Filter duplicates and split output into a main and an excess output
    BufferBuilder mainBuilder = _merge_store->getBufferBuilder(_size_limit);
    BufferBuilder excessBuilder = _merge_store->getBufferBuilder(INT32_MAX);
    tsl::robin_set<Mallob::Clause, Mallob::NonCommutativeClauseHasher, Mallob::SortedClauseExactEquals> mergedClauseSet;
    BufferReader storeOutputReader = _merge_store->getBufferReader(storeOutput.data(), storeOutput.size());
    BufferBuilder* currentBuilder = &mainBuilder;
    int excessFirstCounterPosition = -1;
    while (true) {
        Mallob::Clause c = storeOutputReader.getNextIncomingClause();
        if (!c.begin) break;
        // Clause not seen before?
        if (!mergedClauseSet.count(c)) {
            mergedClauseSet.insert(c);
            // try to insert into current output
            if (!currentBuilder->append(c) && currentBuilder == &mainBuilder) {
                // main output full - switch from main output to excess output
                currentBuilder = &excessBuilder;
                if (currentBuilder->append(c))
                    excessFirstCounterPosition = currentBuilder->getCurrentCounterPosition();
            }
        }
    }

    // Extract built buffers for main and excess output
    auto resultClauses = mainBuilder.extractBuffer();
    excessOut = excessBuilder.extractBuffer();

    // Do random tie breaking if necessary
    if (excessFirstCounterPosition != -1) {
        auto failedInfo = mainBuilder.getFailedInsertionInfo();
        if (failedInfo.failedBucket == failedInfo.lastBucket) {
            // Both the main and the excess buffer feature a non-zero number
            // of clauses from this length-LBD bucket: break ties randomly
            redistributeBorderBucketClausesRandomly(resultClauses, excessOut, 
                rng, failedInfo, excessFirstCounterPosition);
        } // else: insertion failed on a bucket border: no tie breaking needed
    }

    return resultClauses;
}

std::vector<int> BufferMerger::merge(std::vector<int>* excessClauses, SplitMix64Rng* rng) {
    
    AbstractClauseThreewayComparator* threewayCompare = _slots_for_sum_of_length_and_lbd ?
        (AbstractClauseThreewayComparator*) new LengthLbdSumClauseThreewayComparator(_max_eff_clause_length+2) :
        (AbstractClauseThreewayComparator*) new LexicographicClauseThreewayComparator();
    ClauseComparator compare(threewayCompare);
    InputClauseComparator inputCompare(threewayCompare);

    // Setup readers
    for (size_t i = 0; i < _readers.size(); i++) {

        // Fetch first clause of this reader
        Clause* c = _readers[i].getCurrentClausePointer();
        _readers[i].getNextIncomingClause();
        if (c->begin == nullptr) continue;
        InputClause inputClause(c, i);

        // Insert clause into merger
        if (_merger.empty()) _merger.insert_after(_merger.before_begin(), inputClause);
        else {
            auto it = _merger.before_begin(); 
            auto nextIt = it; ++nextIt;
            while (nextIt != _merger.end() && inputCompare(inputClause, *nextIt)) {
                ++it;
                ++nextIt;
            }
            _merger.insert_after(it, inputClause);
        }
    }

    // Setup builders for main buffer and excess clauses buffer
    BufferBuilder mainBuilder(_size_limit, _max_eff_clause_length, _slots_for_sum_of_length_and_lbd);
    mainBuilder.setFreeClauseLengthLimit(_max_free_eff_clause_length - ClauseMetadata::numInts());
    BufferBuilder* excessBuilder {nullptr};
    if (excessClauses != nullptr) {
        excessBuilder = new BufferBuilder(_size_limit, _max_eff_clause_length, _slots_for_sum_of_length_and_lbd);
    }
    BufferBuilder* currentBuilder = &mainBuilder;
    int excessFirstCounterPosition = -1;

    // For checking duplicates
    Clause lastSeenClause;
    tsl::robin_set<Mallob::Clause, Mallob::NonCommutativeClauseHasher, Mallob::SortedClauseExactEquals> acceptedClausesSet;
    int currentClauseLengthOfSet = 0;

    // Merge rounds
    while (!_merger.empty()) {

        // Fetch next best clause
        auto& [clause, readerId] = _merger.front();
        
        // Duplicate?
        if (currentClauseLengthOfSet == clause->size && acceptedClausesSet.contains(*clause)) {
            // Duplicate! Either lastSeenClause == clause or the clause was seen before with another LBD.
        } else {
            // -- not a duplicate
            lastSeenClause = *clause;
            // insert to set which is needed for filtering out identical clauses with different LBDs 
            if (currentClauseLengthOfSet < lastSeenClause.size) {
                // new clause length reached: can safely discard smaller accepted clauses
                acceptedClausesSet.clear();
                currentClauseLengthOfSet = lastSeenClause.size;
            }
            acceptedClausesSet.insert(lastSeenClause);

            // Try to append to current builder
            bool success = currentBuilder->append(lastSeenClause);
            if (!success && currentBuilder == &mainBuilder) {
                // Switch from normal output to excess clauses output
                assert(excessBuilder);
                currentBuilder = excessBuilder;
                success = currentBuilder->append(lastSeenClause);
                if (success) excessFirstCounterPosition = currentBuilder->getCurrentCounterPosition();
            }
        }

        // Refill merger
        _readers[readerId].getNextIncomingClause();
        if (clause->begin == nullptr) {
            // No clauses left for this reader
            _merger.erase_after(_merger.before_begin());
        } else {
            // Insert clause at the correct position in the merger
            auto it = _merger.begin(); 
            auto nextIt = it; ++nextIt;
            while (nextIt != _merger.end() && inputCompare(_merger.front(), *nextIt)) {
                ++it;
                ++nextIt;
            }
            if (it != _merger.begin()) {
                // Move element
                auto elem = _merger.front();
                _merger.erase_after(_merger.before_begin());
                _merger.insert_after(it, elem);
            } // Else: element is already at the right place
        }
    }

    auto resultClauses = mainBuilder.extractBuffer();

    // Fill provided excess clauses buffer with result from according builder
    if (excessClauses != nullptr) {
        *excessClauses = excessBuilder->extractBuffer();

        if (rng != nullptr && excessFirstCounterPosition != -1) {
            // Do random tie breaking if necessary
            auto failedInfo = mainBuilder.getFailedInsertionInfo();
            if (failedInfo.failedBucket == failedInfo.lastBucket) {
                // Both the main and the excess buffer feature a non-zero number
                // of clauses from this length-LBD bucket: break ties randomly
                redistributeBorderBucketClausesRandomly(resultClauses, *excessClauses, 
                    *rng, failedInfo, excessFirstCounterPosition);
            } // else: insertion failed on a bucket border: no tie breaking needed
        }

        delete excessBuilder;
    }

    delete threewayCompare;
    return resultClauses;
}

std::string vecToStr(const std::vector<int>& vec) {
    std::string out;
    for (auto elem : vec) out += std::to_string(elem) + " ";
    return out.substr(0, out.size()-1);
}

void BufferMerger::redistributeBorderBucketClausesRandomly(std::vector<int>& resultClauses, std::vector<int>& excessClauses, 
        SplitMix64Rng& rng, const BufferBuilder::FailedInsertion& failedInfo, int excess1stCounterPos) {

    const int clslen = failedInfo.failedBucket.clauseLength;
    const int countPosResult = failedInfo.lastCounterPosition;
    const int countPosExcess = excess1stCounterPos;

    const int nbClausesResult = resultClauses[countPosResult];
    const int nbClausesExcess = excessClauses[countPosExcess];
    assert(nbClausesResult > 0);
    assert(nbClausesExcess > 0);

    int nbClausesLeft = nbClausesResult + nbClausesExcess;
    int nbClausesToSelect = nbClausesResult;

    std::vector<int> clausesForResultBuffer;
    std::vector<int> clausesForExcessBuffer;

    LOG(V5_DEBG, "bucket (%i,%i): re-select %i clauses in result, %i clauses in excess\n",
        clslen, failedInfo.failedBucket.lbd, nbClausesResult, nbClausesExcess);

    // Iterate over all n clauses (first k from main, then n-k from excess)
    // and select k from n clauses which should go into main
    for (int cc = 0; cc < nbClausesResult+nbClausesExcess; cc++) {

        int* cBegin = cc < nbClausesResult ? 
            resultClauses.data() + countPosResult + 1 + cc*clslen : 
            excessClauses.data() + countPosExcess + 1 + (cc-nbClausesResult)*clslen;

        if (select_next_for_k_from_n(nbClausesToSelect, nbClausesLeft, rng)) {
            // Selected -- move to main!
            nbClausesToSelect--;
            clausesForResultBuffer.insert(clausesForResultBuffer.end(), cBegin, cBegin+clslen);
        } else {
            // Not selected -- move to excess!
            clausesForExcessBuffer.insert(clausesForExcessBuffer.end(), cBegin, cBegin+clslen);
        }
        nbClausesLeft--;
    }
    assert(nbClausesLeft == 0);
    assert(nbClausesToSelect == 0);
    assert(clausesForResultBuffer.size() == nbClausesResult * clslen);
    assert(clausesForExcessBuffer.size() == nbClausesExcess * clslen);

    //LOG(V2_INFO, "Result buf before: %s\n", vecToStr(resultClauses).c_str());
    //LOG(V2_INFO, "Excess buf before: %s\n", vecToStr(excessClauses).c_str());
    //LOG(V2_INFO, "Clauses for result: %s\n", vecToStr(clausesForResultBuffer).c_str());
    //LOG(V2_INFO, "Clauses for excess: %s\n", vecToStr(clausesForExcessBuffer).c_str());

    // Copy clause data into respective buffers
    memcpy(resultClauses.data() + countPosResult + 1, clausesForResultBuffer.data(), sizeof(int) * clausesForResultBuffer.size());
    memcpy(excessClauses.data() + countPosExcess + 1, clausesForExcessBuffer.data(), sizeof(int) * clausesForExcessBuffer.size());

    //LOG(V2_INFO, "Result buf after: %s\n", vecToStr(resultClauses).c_str());
    //LOG(V2_INFO, "Excess buf after: %s\n", vecToStr(excessClauses).c_str());
}
