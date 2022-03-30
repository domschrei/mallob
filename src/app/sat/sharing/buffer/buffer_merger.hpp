
#pragma once

#include <vector>
#include <set>
#include <forward_list>

#include "buffer_builder.hpp"
#include "buffer_reader.hpp"
#include "../filter/clause_filter.hpp" 

class BufferMerger {
    
private:
    int _size_limit;
    int _max_clause_length;
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

public:
    BufferMerger(int sizeLimit, int maxClauseLength, bool slotsForSumOfLengthAndLbd, bool useChecksum = false);
    void add(BufferReader&& reader);
    std::vector<int> merge(std::vector<int>* excessClauses = nullptr);
    
private:
    std::vector<int> fastMerge(int sizeLimit, std::vector<int>* excessClauses = nullptr);
};
