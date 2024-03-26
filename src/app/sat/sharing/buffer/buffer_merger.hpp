
#pragma once

#include <forward_list>                        // for forward_list
#include <utility>                             // for pair
#include <vector>                              // for vector
#include "app/sat/data/clause.hpp"             // for Clause
#include "app/sat/data/clause_comparison.hpp"  // for AbstractClauseThreeway...
#include "buffer_builder.hpp"                  // for BufferBuilder
#include "buffer_reader.hpp"                   // for BufferReader
class Parameters;
class SplitMix64Rng;
template <bool Concurrent> class StaticClauseStore;

class BufferMerger {
    
private:
    int _size_limit;
    int _max_eff_clause_length;
    int _max_free_eff_clause_length;
    int _slots_for_sum_of_length_and_lbd;

    bool _use_checksum;
    std::vector<BufferReader> _readers;

    std::vector<Clause> _next_clauses;
    std::vector<bool> _selected;

    typedef std::pair<Clause*, int> InputClause;
    struct InputClauseComparator {
        AbstractClauseThreewayComparator* compare;
        InputClauseComparator(AbstractClauseThreewayComparator* compare) : compare(compare) {}
        bool operator()(const InputClause& left, const InputClause& right) const {
            int res = compare->compare(*left.first, *right.first);
            if (res != 0) return res > 0;
            if (left.second != right.second) return left.second < right.second;
            return false;
        }
    };
    std::forward_list<InputClause> _merger;
    StaticClauseStore<false>* _merge_store {nullptr};

public:
    BufferMerger(int sizeLimit, int maxEffClauseLength, int maxFreeEffClauseLength, bool slotsForSumOfLengthAndLbd, bool useChecksum = false);
    BufferMerger(StaticClauseStore<false>* mergeStore, int sizeLimit, int maxEffClauseLength, bool slotsForSumOfLengthAndLbd, bool useChecksum = false);
    void add(BufferReader&& reader);

    std::vector<int> mergeDiscardingExcess();
    std::vector<int> mergePreservingExcess(std::vector<int>& excessOut);
    std::vector<int> mergePreservingExcessWithRandomTieBreaking(std::vector<int>& excessOut, SplitMix64Rng& rng);

    std::vector<int> mergePriorityBased(const Parameters& params, std::vector<int>& excessOut, SplitMix64Rng& rng);
    
private:
    std::vector<int> merge(std::vector<int>* excessClauses, SplitMix64Rng* rng);
    void redistributeBorderBucketClausesRandomly(std::vector<int>& resultClauses, std::vector<int>& excessClauses, 
        SplitMix64Rng& rng, const BufferBuilder::FailedInsertion& failedInfo, int excess1stCounterPos);
};
